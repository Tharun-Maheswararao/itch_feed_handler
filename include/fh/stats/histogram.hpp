// Fixed-size log-linear latency histogram (HDR-histogram style).
//
// Values below 2^kSubBits land in exact unit-width buckets. Above that, each
// power of two is split into 2^kSubBits linear sub-buckets, so the relative
// error is bounded by 1/2^kSubBits (~3% with kSubBits=5). record() is a
// count-leading-zeros, a shift, and an increment: no allocation, no branches
// on the hot path apart from the max update.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace fh {

class LatencyHistogram {
public:
    static constexpr int kSubBits = 5;
    static constexpr uint64_t kSub = uint64_t{1} << kSubBits;
    static constexpr int kMaxExp = 48;  // values up to 2^48 ticks
    static constexpr std::size_t kBuckets = kSub * (kMaxExp - kSubBits + 2);

    static constexpr std::size_t index_of(uint64_t v) noexcept {
        if (v < kSub) return static_cast<std::size_t>(v);
        const int msb = 63 - __builtin_clzll(v);  // >= kSubBits
        if (msb > kMaxExp) return kBuckets - 1;
        const int shift = msb - kSubBits;
        const uint64_t sub = (v >> shift) & (kSub - 1);
        return static_cast<std::size_t>((shift + 1) * kSub + sub);
    }

    // Inclusive lower bound of bucket i.
    static constexpr uint64_t lower_bound(std::size_t i) noexcept {
        if (i < kSub) return i;
        const uint64_t shift = i / kSub - 1;
        const uint64_t sub = i % kSub;
        return (kSub + sub) << shift;
    }
    // Exclusive upper bound of bucket i.
    static constexpr uint64_t upper_bound(std::size_t i) noexcept {
        if (i < kSub) return i + 1;
        const uint64_t shift = i / kSub - 1;
        return lower_bound(i) + (uint64_t{1} << shift);
    }

    void record(uint64_t v) noexcept {
        ++counts_[index_of(v)];
        ++total_;
        sum_ += v;
        max_ = std::max(max_, v);
        min_ = std::min(min_, v);
    }

    void merge(const LatencyHistogram& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

    // Value at quantile q in [0,1]. Reports the highest value the containing
    // bucket can hold (never under-reports), clamped to the observed max.
    uint64_t percentile(double q) const noexcept {
        if (total_ == 0) return 0;
        const auto rank = static_cast<uint64_t>(q * static_cast<double>(total_ - 1)) + 1;
        uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min(upper_bound(i) - 1, max_);
        }
        return max_;
    }

    uint64_t count() const noexcept { return total_; }
    uint64_t max() const noexcept { return max_; }
    uint64_t min() const noexcept { return total_ ? min_ : 0; }
    double mean() const noexcept { return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0; }
    uint64_t bucket_count(std::size_t i) const noexcept { return counts_[i]; }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t total_ = 0;
    uint64_t sum_ = 0;
    uint64_t max_ = 0;
    uint64_t min_ = std::numeric_limits<uint64_t>::max();
};

}  // namespace fh
