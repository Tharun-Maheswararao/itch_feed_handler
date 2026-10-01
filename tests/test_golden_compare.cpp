// Golden-model comparison on a synthetic feed: the optimized book and the
// two-thread pipeline must equal the single-threaded golden book after every
// message, checked with a running hash of book state.
#include <gtest/gtest.h>

#include <array>
#include <stdexcept>

#include "fh/book/compare.hpp"
#include "fh/book/fast_book.hpp"
#include "fh/book/golden_book.hpp"
#include "fh/pipeline.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;

namespace {
const std::vector<uint8_t>& feed() {
    static const auto f = synth::synthetic_feed(600'000, 1234, 24);
    return f;
}
}  // namespace

// Lock-step: apply each message to both books and compare after every one.
TEST(GoldenCompare, FastBookEqualsGoldenAfterEveryMessage) {
    auto g = std::make_unique<GoldenBook<true>>();
    auto f = std::make_unique<FastBook<true>>(1 << 12);  // small: forces grows
    uint64_t n = 0, first_bad = 0;
    std::string err;
    parse_buffer(feed().data(), feed().data() + feed().size(), [&](const Msg& m) {
        apply(*g, m);
        apply(*f, m);
        ++n;
        if (!first_bad && g->hash() != f->hash()) first_bad = n;
        if (n % 50'000 == 0) {
            ASSERT_TRUE(books_equal(*g, *f, &err)) << "after " << n << ": " << err;
            ASSERT_TRUE(g->check_invariants(&err)) << err;
            ASSERT_TRUE(f->check_invariants(&err)) << err;
        }
    });
    EXPECT_EQ(first_bad, 0u) << "hash diverged at book message " << first_bad;
    EXPECT_TRUE(books_equal(*g, *f, &err)) << err;
    EXPECT_GT(f->order_map_grows(), 0u);
    EXPECT_GT(g->counters().unknown_ref, 0u) << "feed should include unknown references";
    EXPECT_GT(g->counters().replaces, 0u);
}

template <class Book>
void pipeline_matches_golden(Book& book) {
    auto g = std::make_unique<GoldenBook<true>>();
    const RunResult rg = run_single(feed().data(), feed().data() + feed().size(), *g, true, 4096);
    PipelineOptions opt;
    opt.verify = true;
    opt.checkpoint_every = 4096;
    const RunResult rp = run_pipeline(feed().data(), feed().data() + feed().size(), book, opt);
    EXPECT_EQ(rp.parse.counts, rg.parse.counts);
    EXPECT_EQ(rp.book_messages, rg.book_messages);
    EXPECT_EQ(rp.checkpoints, rg.checkpoints);
    EXPECT_EQ(rp.digest, rg.digest);
    EXPECT_EQ(rp.total.count(), rp.book_messages);
    std::string err;
    EXPECT_TRUE(books_equal(*g, book, &err)) << err;
    EXPECT_TRUE(book.check_invariants(&err)) << err;
}

TEST(GoldenCompare, PipelineWithFastBookMatchesGolden) {
    auto b = std::make_unique<FastBook<true>>(1 << 16);
    pipeline_matches_golden(*b);
}

TEST(GoldenCompare, PipelineWithBatchedRingMatchesGolden) {
    for (std::size_t k : {8u, 32u}) {
        auto g = std::make_unique<GoldenBook<true>>();
        const RunResult rg = run_single(feed().data(), feed().data() + feed().size(), *g, true);
        auto b = std::make_unique<FastBook<true>>(1 << 16);
        PipelineOptions opt;
        opt.verify = true;
        opt.ring_batch = k;
        const RunResult rp = run_pipeline(feed().data(), feed().data() + feed().size(), *b, opt);
        EXPECT_EQ(rp.ring_batch, k);
        EXPECT_EQ(rp.digest, rg.digest) << "ring batch " << k;
        std::string err;
        EXPECT_TRUE(books_equal(*g, *b, &err)) << err;
    }
}

// Sampled latency: the book is untouched, about 1 in N messages is timed,
// and every timed message also records a ring depth.
TEST(GoldenCompare, SampledLatencyPipelineMatchesGolden) {
    auto g = std::make_unique<GoldenBook<true>>();
    const RunResult rg = run_single(feed().data(), feed().data() + feed().size(), *g, true);
    for (uint32_t every : {1u, 16u}) {
        for (double rate : {0.0, 4'000'000.0}) {
            auto b = std::make_unique<FastBook<true>>(1 << 16);
            PipelineOptions opt;
            opt.verify = true;
            opt.sample_every = every;
            opt.rate = rate;
            const RunResult rp = run_pipeline(feed().data(), feed().data() + feed().size(), *b, opt);
            EXPECT_EQ(rp.digest, rg.digest) << "sample 1/" << every << " rate " << rate;
            const double expect = static_cast<double>(rp.book_messages) / every;
            EXPECT_NEAR(static_cast<double>(rp.total.count()), expect, expect * 0.05) << every;
            EXPECT_EQ(rp.depth.count(), rp.total.count());
            EXPECT_EQ(rp.book.count(), rp.total.count());
            EXPECT_EQ(rp.sample_every, every);
            if (every == 1) EXPECT_EQ(rp.total.count(), rp.book_messages);
        }
    }
}

TEST(GoldenCompare, SampleRateMustBePowerOfTwo) {
    auto b = std::make_unique<FastBook<true>>(1 << 10);
    PipelineOptions opt;
    opt.sample_every = 12;
    EXPECT_THROW(run_pipeline(feed().data(), feed().data() + 1000, *b, opt), std::invalid_argument);
}

// The sampler must not line up with message position: over every residue
// class of the message index mod 16, the sampled fraction stays ~1/16.
TEST(GoldenCompare, SamplerHasNoPeriodicBias) {
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    constexpr int kN = 16'000'000;
    std::array<int, 16> hits{};
    for (int i = 0; i < kN; ++i)
        if (((xorshift64(rng) >> 40) & 15) == 0) ++hits[i % 16];
    for (int r = 0; r < 16; ++r) EXPECT_NEAR(hits[r], kN / 16 / 16, kN / 16 / 16 * 0.03) << "residue " << r;
}

TEST(GoldenCompare, PipelineWithGoldenBookMatchesGolden) {
    auto b = std::make_unique<GoldenBook<true>>();
    pipeline_matches_golden(*b);
}

TEST(GoldenCompare, PacedPipelineStillMatches) {
    auto g = std::make_unique<GoldenBook<true>>();
    const auto* begin = feed().data();
    const auto* end = begin + std::min<std::size_t>(feed().size(), 400'000);  // ~10k msgs
    const RunResult rg = run_single(begin, end, *g, true);
    auto b = std::make_unique<FastBook<true>>(1 << 16);
    PipelineOptions opt;
    opt.verify = true;
    opt.rate = 2'000'000;  // 2M msgs/s
    opt.ring_batch = 32;    // paced + batched: flush-on-idle must keep it moving
    const RunResult rp = run_pipeline(begin, end, *b, opt);
    EXPECT_EQ(rp.digest, rg.digest);
    std::string err;
    EXPECT_TRUE(books_equal(*g, *b, &err)) << err;
}

TEST(GoldenCompare, LiveViewSnapshotDoesNotChangeResult) {
    auto g = std::make_unique<GoldenBook<true>>();
    const RunResult rg = run_single(feed().data(), feed().data() + feed().size(), *g, true);
    auto b = std::make_unique<FastBook<true>>(1 << 16);
    Seqlock<BookSnapshot> snap;
    PipelineOptions opt;
    opt.verify = true;
    opt.snapshot = &snap;
    opt.view_symbol = "SYM3";
    opt.snapshot_every_pow2 = 1024;
    const RunResult rp = run_pipeline(feed().data(), feed().data() + feed().size(), *b, opt);
    EXPECT_EQ(rp.digest, rg.digest);
    BookSnapshot s;
    ASSERT_TRUE(snap.load(s));
    EXPECT_EQ(std::string(s.symbol, 4), "SYM3");
    EXPECT_GT(s.messages, 0u);
}
