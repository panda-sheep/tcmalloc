// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/huge_cache.h"

#include <tuple>

#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_address_map.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {

template <size_t kEpochs>
void MinMaxTracker<kEpochs>::Report(HugeLength val) {
  timeseries_.Report(val);
}

template <size_t kEpochs>
HugeLength MinMaxTracker<kEpochs>::MaxOverTime(absl::Duration t) const {
  HugeLength m = NHugePages(0);
  size_t num_epochs = ceil(absl::FDivDuration(t, kEpochLength));
  timeseries_.IterBackwards([&](size_t offset, int64_t ts,
                                const Extrema &e) { m = std::max(m, e.max); },
                            num_epochs);
  return m;
}

template <size_t kEpochs>
HugeLength MinMaxTracker<kEpochs>::MinOverTime(absl::Duration t) const {
  HugeLength m = kMaxVal;
  size_t num_epochs = ceil(absl::FDivDuration(t, kEpochLength));
  timeseries_.IterBackwards([&](size_t offset, int64_t ts,
                                const Extrema &e) { m = std::min(m, e.min); },
                            num_epochs);
  return m;
}

template <size_t kEpochs>
void MinMaxTracker<kEpochs>::Print(TCMalloc_Printer *out) const {
  // Prints content of each non-empty epoch, from oldest to most recent data
  const long long millis = absl::ToInt64Milliseconds(kEpochLength);
  out->printf("\nHugeCache Usage Timeseries Stats: window %lldms * %zu", millis,
              kEpochs);

  const absl::Duration max_window =
      kEpochLength * static_cast<const size_t>(kEpochs);
  // clang-format off
  static const absl::Duration kWindows[] = {
    max_window*0.10,
    max_window*0.25,
    max_window*0.50,
    max_window*0.75,
    max_window,
  };
  // clang-format on
  const int kNumWindows = ABSL_ARRAYSIZE(kWindows);
  SeriesStats stats[kNumWindows]{};
  size_t offset_boundary[kNumWindows];
  for (int i = 0; i < kNumWindows; ++i) {
    offset_boundary[i] = ceil(absl::FDivDuration(kWindows[i], kEpochLength));
  }
  // make sure we cannot overflow offset_boundary nor stats
  ASSERT(kEpochs == offset_boundary[kNumWindows - 1]);

  int curr_window = 0;
  timeseries_.IterBackwards(
      [&](size_t offset, int64_t, const Extrema &e) {
        if (e.empty()) return;
        while (offset >= offset_boundary[curr_window]) {
          curr_window++;
        }
        stats[curr_window].Report(offset, e);
      },
      kEpochs);

  out->printf("\n%24s %9s %9s %12s %12s", "report rate", "min", "max", "mean",
              "std_dev");
  // Aggegate and print
  for (int i = 0; i < kNumWindows; ++i) {
    double fraction = absl::FDivDuration(kWindows[i], max_window) * 100;
    out->printf("\nAt %3.0f%% mark:", fraction);
    stats[i].Print(out);

    if (i + 1 == kNumWindows || stats[i + 1].empty()) break;
    stats[i + 1] += stats[i];
  }
  out->printf("\n");
}

template <size_t kEpochs>
void MinMaxTracker<kEpochs>::PrintInPbtxt(PbtxtRegion *hpaa) const {
  // Prints content of each non-empty epoch, from oldest to most recent data
  auto huge_cache_history = hpaa->CreateSubRegion("huge_cache_history");
  huge_cache_history.PrintI64("window_ms",
                              absl::ToInt64Milliseconds(kEpochLength));
  huge_cache_history.PrintI64("epochs", kEpochs);

  timeseries_.Iter(
      [&](size_t offset, int64_t ts, const Extrema &e) {
        auto m = huge_cache_history.CreateSubRegion("measurements");
        m.PrintI64("epoch", offset);
        m.PrintI64("min_bytes", e.min.in_bytes());
        m.PrintI64("max_bytes", e.max.in_bytes());
      },
      timeseries_.kDoNotSkipEmptyEntries);
}

template <size_t kEpochs>
bool MinMaxTracker<kEpochs>::Extrema::operator==(const Extrema &other) const {
  return (other.max == max) && (other.min == min);
}

template <size_t kEpochs>
bool MinMaxTracker<kEpochs>::Extrema::operator!=(const Extrema &other) const {
  return !(this->operator==(other));
}

template <size_t kEpochs>
typename MinMaxTracker<kEpochs>::Extrema &
MinMaxTracker<kEpochs>::Extrema::operator+=(const Extrema &other) {
  max += other.max;
  min += other.min;
  return *this;
}

template <size_t kEpochs>
void MinMaxTracker<kEpochs>::SeriesStats::Report(size_t offset,
                                                 const Extrema &e) {
  max.Report(e.max);
  min.Report(e.min);
  sum += e;
  sum_square += {.min = NHugePages(e.min.raw_num() * e.min.raw_num()),
                 .max = NHugePages(e.max.raw_num() * e.max.raw_num())};
  max_offset = offset;
  count++;
}

template <size_t kEpochs>
typename MinMaxTracker<kEpochs>::SeriesStats &
MinMaxTracker<kEpochs>::SeriesStats::operator+=(SeriesStats &other) {
  ASSERT(max_offset > other.max_offset);
  max.Report(other.max);
  min.Report(other.min);
  sum += other.sum;
  sum_square += other.sum_square;
  count += other.count;
  return *this;
}

template <size_t kEpochs>
void MinMaxTracker<kEpochs>::SeriesStats::Print(TCMalloc_Printer *out) const {
  auto safe_ratio = [](double a, double b) {
    if (b == 0) return 0.0;
    return a / b;
  };

  auto std_dev = [&](size_t sq, size_t s, size_t c) {
    double variance = safe_ratio(sq - safe_ratio(s * s, c), c);
    return std::sqrt(variance);
  };
  out->printf("\n%s %4zu /%4zu %9zu %9zu %12.3f %12.3f", "hugepages_max", count,
              max_offset + 1, max.min.raw_num(), max.max.raw_num(),
              safe_ratio(sum.max.raw_num(), count),
              std_dev(sum_square.max.raw_num(), sum.max.raw_num(), count));
  out->printf("\n%s  =%8.3f %9zu %9zu %12.3f %12.3f", "hugepages_min",
              count / static_cast<double>(max_offset + 1), min.min.raw_num(),
              min.max.raw_num(), safe_ratio(sum.min.raw_num(), count),
              std_dev(sum_square.min.raw_num(), sum.min.raw_num(), count));
}

// Explicit instantiations of template
template class MinMaxTracker<>;
template class MinMaxTracker<600>;

void MovingAverageTracker::Report(HugeLength val) {
  int64_t now = clock_();
  if (rolling_max_average_ < 1 || val >= HugeLength(rolling_max_average_ - 1)) {
    rolling_max_average_ = val.raw_num();
    last_update_ = now;
    last_val_ = val;
    return;
  }
  absl::Duration delta = absl::Nanoseconds(now - last_update_);
  if (delta < kResolution) {
    last_max_ = std::max(last_max_, val);
  } else if (delta < kTimeConstant) {
    while (delta > kResolution) {
      rolling_max_average_ =
          (static_cast<double>(2 * last_max_.raw_num()) +
           rolling_max_average_ * (res_per_time_constant_ - 1)) /
          (res_per_time_constant_ + 1);
      delta -= kResolution;
      last_update_ += absl::ToInt64Nanoseconds(kResolution);
    }
    last_max_ = std::max(last_val_, val);
  } else {
    // Old data is too old
    rolling_max_average_ = std::max(last_val_, val).raw_num();
    last_update_ = now;
  }
  last_val_ = val;
}

HugeLength MovingAverageTracker::RollingMaxAverage() const {
  return NHugePages(rolling_max_average_);
}

// The logic for actually allocating from the cache or backing, and keeping
// the hit rates specified.
HugeRange HugeCache::DoGet(HugeLength n, bool *from_released) {
  auto *node = Find(n);
  if (!node) {
    misses_++;
    weighted_misses_ += n.raw_num();
    HugeRange res = allocator_->Get(n);
    if (res.valid()) {
      *from_released = true;
    }

    return res;
  }
  hits_++;
  weighted_hits_ += n.raw_num();
  *from_released = false;
  size_ -= n;
  UpdateSize(size());
  HugeRange result, leftover;
  // Put back whatever we have left (or nothing, if it's exact.)
  std::tie(result, leftover) = Split(node->range(), n);
  cache_.Remove(node);
  if (leftover.valid()) {
    cache_.Insert(leftover);
  }
  return result;
}

void HugeCache::MaybeGrowCacheLimit(HugeLength missed) {
  // Our goal is to make the cache size = the largest "brief dip."
  //
  // A "dip" being a case where usage shrinks, then increases back up
  // to previous levels (at least partially).
  //
  // "brief" is "returns to normal usage in < kCacheTime." (In
  // other words, we ideally want to be willing to cache memory for
  // kCacheTime before expecting it to be used again--we are loose
  // on the timing..)
  //
  // The interesting part is finding those dips.

  // This is the downward slope: we lost some usage. (This in theory could
  // be as much as 2 * kCacheTime old, which is fine.)
  const HugeLength shrink = off_peak_tracker_.MaxOverTime(kCacheTime);

  // This is the upward slope: we are coming back up.
  const HugeLength grow = usage_ - usage_tracker_.MinOverTime(kCacheTime);

  // Ideally we now know that we dipped down by some amount, then came
  // up.  Sadly our stats aren't quite good enough to guarantee things
  // happened in the proper order.  Suppose our usage takes the
  // following path (in essentially zero time):
  // 0, 10000, 5000, 5500.
  //
  // Clearly the proven dip here is 500.  But we'll compute shrink = 5000,
  // grow = 5500--we'd prefer to measure from a min *after* that shrink.
  //
  // It's difficult to ensure this, and hopefully this case is rare.
  // TODO(b/134690209): figure out if we can solve that problem.
  const HugeLength dip = std::min(shrink, grow);

  // Fragmentation: we may need to cache a little more than the actual
  // usage jump. 10% seems to be a reasonable addition that doesn't waste
  // much space, but gets good performance on tests.
  const HugeLength slack = dip / 10;

  const HugeLength lim = dip + slack;

  if (lim > limit()) {
    last_limit_change_ = clock_();
    limit_ = lim;
  }
}

void HugeCache::IncUsage(HugeLength n) {
  usage_ += n;
  usage_tracker_.Report(usage_);
  detailed_tracker_.Report(usage_);
  off_peak_tracker_.Report(NHugePages(0));
  if (size() + usage() > max_rss_) max_rss_ = size() + usage();
}

void HugeCache::DecUsage(HugeLength n) {
  usage_ -= n;
  usage_tracker_.Report(usage_);
  detailed_tracker_.Report(usage_);
  const HugeLength max = usage_tracker_.MaxOverTime(kCacheTime);
  ASSERT(max >= usage_);
  const HugeLength off_peak = max - usage_;
  off_peak_tracker_.Report(off_peak);
  if (size() + usage() > max_rss_) max_rss_ = size() + usage();
}

void HugeCache::UpdateSize(HugeLength size) {
  size_tracker_.Report(size);
  if (size > max_size_) max_size_ = size;
  if (size + usage() > max_rss_) max_rss_ = size + usage();

  // TODO(b/134691947): moving this inside the MinMaxTracker would save one call
  // to clock_() but all MinMaxTrackers would track regret instead.
  int64_t now = clock_();
  if (now > last_regret_update_) {
    regret_ += size.raw_num() * (now - last_regret_update_);
    last_regret_update_ = now;
  }
}

HugeRange HugeCache::Get(HugeLength n, bool *from_released) {
  HugeRange r = DoGet(n, from_released);
  // failure to get a range should "never" "never" happen (VSS limits
  // or wildly incorrect allocation sizes only...) Don't deal with
  // this case for cache size accounting.
  IncUsage(r.len());

  const bool miss = r.valid() && *from_released;
  if (miss) MaybeGrowCacheLimit(n);
  return r;
}

void HugeCache::Release(HugeRange r) {
  DecUsage(r.len());

  cache_.Insert(r);
  size_ += r.len();
  if (size_ <= limit()) {
    fills_++;
  } else {
    overflows_++;
  }

  // Shrink the limit, if we're going to do it, before we shrink to
  // the max size.  (This could reduce the number of regions we break
  // in half to avoid overshrinking.)
  if (absl::Nanoseconds(clock_() - last_limit_change_) > (kCacheTime * 2)) {
    total_fast_unbacked_ += MaybeShrinkCacheLimit();
  }
  total_fast_unbacked_ += ShrinkCache(limit());

  UpdateSize(size());
}

void HugeCache::ReleaseUnbacked(HugeRange r) {
  DecUsage(r.len());
  // No point in trying to cache it, just hand it back.
  allocator_->Release(r);
}

HugeLength HugeCache::MaybeShrinkCacheLimit() {
  last_limit_change_ = clock_();

  const HugeLength min = size_tracker_.MinOverTime(kCacheTime * 2);
  // If cache size has gotten down to at most 20% of max, we assume
  // we're close enough to the optimal size--we don't want to fiddle
  // too much/too often unless we have large gaps in usage.
  if (min < limit() / 5) return NHugePages(0);

  // Take away half of the unused portion.
  HugeLength drop = std::max(min / 2, NHugePages(1));
  limit_ = std::max(limit() <= drop ? NHugePages(0) : limit() - drop,
                    MinCacheLimit());
  return ShrinkCache(limit());
}

HugeLength HugeCache::ShrinkCache(HugeLength target) {
  HugeLength removed = NHugePages(0);
  while (size_ > target) {
    // Remove smallest-ish nodes, to avoid fragmentation where possible.
    auto *node = Find(NHugePages(1));
    CHECK_CONDITION(node);
    HugeRange r = node->range();
    cache_.Remove(node);
    // Suppose we're 10 MiB over target but the smallest available node
    // is 100 MiB.  Don't go overboard--split up the range.
    // In particular - this prevents disastrous results if we've decided
    // the cache should be 99 MiB but the actual hot usage is 100 MiB
    // (and it is unfragmented).
    const HugeLength delta = size() - target;
    if (r.len() > delta) {
      HugeRange to_remove, leftover;
      std::tie(to_remove, leftover) = Split(r, delta);
      ASSERT(leftover.valid());
      cache_.Insert(leftover);
      r = to_remove;
    }

    size_ -= r.len();
    // Note, actual unback implementation is temporarily dropping and
    // re-acquiring the page heap lock here.
    unback_(r.start_addr(), r.byte_len());
    allocator_->Release(r);
    removed += r.len();
  }

  return removed;
}

HugeLength HugeCache::ReleaseCachedPages(HugeLength n) {
  // This is a good time to check: is our cache going persistently unused?
  HugeLength released = MaybeShrinkCacheLimit();

  if (released < n) {
    n -= released;
    const HugeLength target = n > size() ? NHugePages(0) : size() - n;
    released += ShrinkCache(target);
  }

  UpdateSize(size());
  total_periodic_unbacked_ += released;
  return released;
}

void HugeCache::AddSpanStats(SmallSpanStats *small, LargeSpanStats *large,
                             PageAgeHistograms *ages) const {
  CHECK_CONDITION(kPagesPerHugePage >= kMaxPages);
  for (const HugeAddressMap::Node *node = cache_.first(); node != nullptr;
       node = node->next()) {
    HugeLength n = node->range().len();
    if (large != nullptr) {
      large->spans++;
      large->normal_pages += n.in_pages();
    }

    if (ages != nullptr) {
      ages->RecordRange(n.in_pages(), false, node->when());
    }
  }
}

HugeAddressMap::Node *HugeCache::Find(HugeLength n) {
  HugeAddressMap::Node *curr = cache_.root();
  // invariant: curr != nullptr && curr->longest >= n
  // we favor smaller gaps and lower nodes and lower addresses, in that
  // order. The net effect is that we are neither a best-fit nor a
  // lowest-address allocator but vaguely close to both.
  HugeAddressMap::Node *best = nullptr;
  while (curr && curr->longest() >= n) {
    if (curr->range().len() >= n) {
      if (!best || best->range().len() > curr->range().len()) {
        best = curr;
      }
    }

    // Either subtree could contain a better fit and we don't want to
    // search the whole tree. Pick a reasonable child to look at.
    auto left = curr->left();
    auto right = curr->right();
    if (!left || left->longest() < n) {
      curr = right;
      continue;
    }

    if (!right || right->longest() < n) {
      curr = left;
      continue;
    }

    // Here, we have a nontrivial choice.
    if (left->range().len() == right->range().len()) {
      if (left->longest() <= right->longest()) {
        curr = left;
      } else {
        curr = right;
      }
    } else if (left->range().len() < right->range().len()) {
      // Here, the longest range in both children is the same...look
      // in the subtree with the smaller root, as that's slightly
      // more likely to be our best.
      curr = left;
    } else {
      curr = right;
    }
  }
  return best;
}

void HugeCache::Print(TCMalloc_Printer *out) {
  const long long millis = absl::ToInt64Milliseconds(kCacheTime);
  out->printf(
      "HugeCache: contains unused, backed hugepage(s) "
      "(kCacheTime = %lldms)\n",
      millis);
  // a / (a + b), avoiding division by zero
  auto safe_ratio = [](double a, double b) {
    const double total = a + b;
    if (total == 0) return 0.0;
    return a / total;
  };

  const double hit_rate = safe_ratio(hits_, misses_);
  const double overflow_rate = safe_ratio(overflows_, fills_);

  out->printf(
      "HugeCache: %zu / %zu hugepages cached / cache limit "
      "(%.3f hit rate, %.3f overflow rate)\n",
      size_.raw_num(), limit().raw_num(), hit_rate, overflow_rate);
  out->printf("HugeCache: %zu MiB fast unbacked, %zu MiB periodic\n",
              total_fast_unbacked_.in_bytes() / 1024 / 1024,
              total_periodic_unbacked_.in_bytes() / 1024 / 1024);
  UpdateSize(size());
  out->printf("HugeCache: %zu MiB*s cached since startup\n",
              NHugePages(regret_).in_mib() / 1000 / 1000 / 1000);

  usage_tracker_.Report(usage_);
  const HugeLength usage_min = usage_tracker_.MinOverTime(kCacheTime);
  const HugeLength usage_max = usage_tracker_.MaxOverTime(kCacheTime);
  out->printf(
      "HugeCache: recent usage range: %zu min - %zu curr -  %zu max MiB\n",
      usage_min.in_mib(), usage_.in_mib(), usage_max.in_mib());

  const HugeLength off_peak = usage_max - usage_;
  off_peak_tracker_.Report(off_peak);
  const HugeLength off_peak_min = off_peak_tracker_.MinOverTime(kCacheTime);
  const HugeLength off_peak_max = off_peak_tracker_.MaxOverTime(kCacheTime);
  out->printf(
      "HugeCache: recent offpeak range: %zu min - %zu curr - %zu max MiB\n",
      off_peak_min.in_mib(), off_peak.in_mib(), off_peak_max.in_mib());

  const HugeLength cache_min = size_tracker_.MinOverTime(kCacheTime);
  const HugeLength cache_max = size_tracker_.MaxOverTime(kCacheTime);
  out->printf(
      "HugeCache: recent cache range: %zu min - %zu curr - %zu max MiB\n",
      cache_min.in_mib(), size_.in_mib(), cache_max.in_mib());

  detailed_tracker_.Print(out);
}

void HugeCache::PrintInPbtxt(PbtxtRegion *hpaa) {
  hpaa->PrintI64("huge_cache_time_const",
                 absl::ToInt64Milliseconds(kCacheTime));

  // a / (a + b), avoiding division by zero
  auto safe_ratio = [](double a, double b) {
    const double total = a + b;
    if (total == 0) return 0.0;
    return a / total;
  };

  const double hit_rate = safe_ratio(hits_, misses_);
  const double overflow_rate = safe_ratio(overflows_, fills_);

  // number of bytes in HugeCache
  hpaa->PrintI64("cached_huge_page_bytes", size_.raw_num() * kPageSize);
  // max allowed bytes in HugeCache
  hpaa->PrintI64("max_cached_huge_page_bytes", limit().raw_num() * kPageSize);
  // lifetime cache hit rate
  hpaa->PrintDouble("huge_cache_hit_rate", hit_rate);
  // lifetime cache overflow rate
  hpaa->PrintDouble("huge_cache_overflow_rate", overflow_rate);
  // bytes eagerly unbacked by HugeCache
  hpaa->PrintI64("fast_unbacked_bytes", total_fast_unbacked_.in_bytes());
  // bytes unbacked by periodic releaser thread
  hpaa->PrintI64("periodic_unbacked_bytes",
                 total_periodic_unbacked_.in_bytes());
  UpdateSize(size());
  // memory cached since startup (in MiB*s)
  hpaa->PrintI64("huge_cache_regret",
                 NHugePages(regret_).in_mib() / 1000 / 1000 / 1000);

  usage_tracker_.Report(usage_);
  const HugeLength usage_min = usage_tracker_.MinOverTime(kCacheTime);
  const HugeLength usage_max = usage_tracker_.MaxOverTime(kCacheTime);
  {
    auto usage_stats = hpaa->CreateSubRegion("huge_cache_usage_stats");
    usage_stats.PrintI64("min_bytes", usage_min.in_bytes());
    usage_stats.PrintI64("current_bytes", usage_.in_bytes());
    usage_stats.PrintI64("max_bytes", usage_max.in_bytes());
  }

  const HugeLength off_peak = usage_max - usage_;
  off_peak_tracker_.Report(off_peak);
  const HugeLength off_peak_min = off_peak_tracker_.MinOverTime(kCacheTime);
  const HugeLength off_peak_max = off_peak_tracker_.MaxOverTime(kCacheTime);
  {
    auto usage_stats = hpaa->CreateSubRegion("huge_cache_offpeak_stats");
    usage_stats.PrintI64("min_bytes", off_peak_min.in_bytes());
    usage_stats.PrintI64("current_bytes", off_peak.in_bytes());
    usage_stats.PrintI64("max_bytes", off_peak_max.in_bytes());
  }

  const HugeLength cache_min = size_tracker_.MinOverTime(kCacheTime);
  const HugeLength cache_max = size_tracker_.MaxOverTime(kCacheTime);
  {
    auto usage_stats = hpaa->CreateSubRegion("huge_cache_cache_stats");
    usage_stats.PrintI64("min_bytes", cache_min.in_bytes());
    usage_stats.PrintI64("current_bytes", size_.in_bytes());
    usage_stats.PrintI64("max_bytes", cache_max.in_bytes());
  }

  detailed_tracker_.PrintInPbtxt(hpaa);
}

}  // namespace tcmalloc
