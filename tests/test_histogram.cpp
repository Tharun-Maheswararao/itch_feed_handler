#include <gtest/gtest.h>

#include <random>

#include "fh/stats/histogram.hpp"

using namespace fh;
using H = LatencyHistogram;

TEST(Histogram, BucketsAreContiguousAndCoverRange) {
    for (std::size_t i = 0; i + 1 < H::kBuckets; ++i) {
        EXPECT_EQ(H::upper_bound(i), H::lower_bound(i + 1)) << i;
        EXPECT_LT(H::lower_bound(i), H::upper_bound(i));
    }
    EXPECT_EQ(H::lower_bound(0), 0u);
}

TEST(Histogram, IndexOfRoundTrips) {
    std::mt19937_64 rng(1);
    for (int i = 0; i < 100'000; ++i) {
        const uint64_t v = rng() >> (rng() % 40 + 17);  // spread over magnitudes
        const auto b = H::index_of(v);
        ASSERT_LE(H::lower_bound(b), v);
        ASSERT_LT(v, H::upper_bound(b));
    }
    for (uint64_t v = 0; v < 5000; ++v) {
        const auto b = H::index_of(v);
        ASSERT_LE(H::lower_bound(b), v);
        ASSERT_LT(v, H::upper_bound(b));
    }
}

TEST(Histogram, SmallValuesAreExact) {
    H h;
    for (uint64_t v = 0; v < 32; ++v) h.record(v);
    EXPECT_EQ(h.percentile(0.0), 0u);
    EXPECT_EQ(h.percentile(1.0), 31u);
    EXPECT_EQ(h.max(), 31u);
    EXPECT_EQ(h.count(), 32u);
}

TEST(Histogram, PercentilesWithinRelativeError) {
    H h;
    for (uint64_t v = 1; v <= 1'000'000; ++v) h.record(v);
    const double tol = 1.0 / H::kSub;  // one sub-bucket
    for (double q : {0.5, 0.9, 0.99, 0.999}) {
        const double exact = q * 1'000'000;
        const double got = static_cast<double>(h.percentile(q));
        EXPECT_GE(got, exact * (1 - 1e-6)) << "never under-reports, q=" << q;
        EXPECT_LE(got, exact * (1 + tol)) << q;
    }
    EXPECT_EQ(h.percentile(1.0), 1'000'000u);
    EXPECT_NEAR(h.mean(), 500'000.5, 1e-6);
}

TEST(Histogram, TailIsVisible) {
    H h;
    for (int i = 0; i < 999'000; ++i) h.record(100);
    for (int i = 0; i < 1'000; ++i) h.record(1'000'000);
    EXPECT_LE(h.percentile(0.99), 103u);
    EXPECT_GE(h.percentile(0.9995), 1'000'000u * 31 / 32);
    EXPECT_EQ(h.max(), 1'000'000u);
}

TEST(Histogram, MergeAddsCounts) {
    H a, b;
    a.record(5);
    b.record(500);
    a.merge(b);
    EXPECT_EQ(a.count(), 2u);
    EXPECT_EQ(a.max(), 500u);
    EXPECT_EQ(a.min(), 5u);
}

TEST(Histogram, HugeValuesClampToLastBucket) {
    H h;
    h.record(~uint64_t{0});
    EXPECT_EQ(h.bucket_count(H::kBuckets - 1), 1u);
}
