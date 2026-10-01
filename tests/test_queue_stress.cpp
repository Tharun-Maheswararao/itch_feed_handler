// The same stress tests for every queue the pipeline can use. A fast queue
// that fails one of these does not count.
//
// FH_STRESS_ITEMS scales the item counts (default sized for CI; the
// ThreadSanitizer job uses fewer). bench/queue_stress.cpp runs the long
// (billions of operations) version.
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <random>
#include <thread>

#include "fh/book/compare.hpp"
#include "fh/book/fast_book.hpp"
#include "fh/book/golden_book.hpp"
#include "fh/pipeline.hpp"
#include "fh/queue/queue_kinds.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;

namespace {

uint64_t scaled(uint64_t def) {
    const char* e = std::getenv("FH_STRESS_ITEMS");
    if (!e) return def;
    const double f = std::strtod(e, nullptr) / 1e6;  // FH_STRESS_ITEMS=1000000 means "1x"
    return std::max<uint64_t>(1000, static_cast<uint64_t>(static_cast<double>(def) * f));
}

// A queue "family": maps a capacity to a concrete queue type.
template <template <class, std::size_t> class Q>
struct Family {
    template <class T, std::size_t C>
    using of = Q<T, C>;
};

using Families = ::testing::Types<Family<SpscAligned>, Family<SpscUnaligned>, Family<MutexQueue>, Family<RigtorpQueue>
#if defined(FH_HAVE_BOOST)
                                  ,
                                  Family<BoostQueue>
#endif
                                  >;

template <class F>
class QueueStress : public ::testing::Test {};
TYPED_TEST_SUITE(QueueStress, Families);

// Producer sends 0..n-1; the consumer checks each value is exactly the
// previous one plus one. Optional random spins create varied interleavings.
template <class Queue>
void sequence_run(uint64_t n, bool random_delays, uint32_t seed = 1) {
    auto q = std::make_unique<Queue>();
    uint64_t errors = 0, last = ~uint64_t{0}, got = 0;
    auto delay = [](std::mt19937& rng) {
        const uint32_t r = rng();
        if ((r & 0xFF) == 0) std::this_thread::yield();  // rare: give up the core
        for (uint32_t i = 0, spins = (r >> 8) & 0x3F; i < spins; ++i) cpu_relax();
    };
    std::thread consumer([&] {
        std::mt19937 rng(seed * 2 + 1);
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t v;
            q->pop(v);
            errors += (v != last + 1);
            last = v;
            ++got;
            if (random_delays) delay(rng);
        }
    });
    std::mt19937 rng(seed * 2);
    for (uint64_t i = 0; i < n; ++i) {
        q->push(i);
        if (random_delays) delay(rng);
    }
    q->flush();
    consumer.join();
    EXPECT_EQ(got, n);
    EXPECT_EQ(errors, 0u);
    EXPECT_EQ(last, n - 1);
    uint64_t v;
    EXPECT_FALSE(q->try_pop(v)) << "queue must be empty afterwards";
}

}  // namespace

TYPED_TEST(QueueStress, SequenceExactlyPlusOne) {
    sequence_run<typename TypeParam::template of<uint64_t, 1024>>(scaled(5'000'000), false);
}

TYPED_TEST(QueueStress, SequenceLargeCapacity) {
    sequence_run<typename TypeParam::template of<uint64_t, 1048576>>(scaled(5'000'000), false);
}

TYPED_TEST(QueueStress, RandomTimingInterleavings) {
    for (uint32_t seed = 1; seed <= 4; ++seed)
        sequence_run<typename TypeParam::template of<uint64_t, 64>>(scaled(250'000), true, seed);
}

// Four slots: indices wrap n/4 times (millions at the default size).
TYPED_TEST(QueueStress, WraparoundFourSlots) {
    sequence_run<typename TypeParam::template of<uint64_t, 4>>(scaled(8'000'000), false);
}

TYPED_TEST(QueueStress, WraparoundFourSlotsRandomTiming) {
    sequence_run<typename TypeParam::template of<uint64_t, 4>>(scaled(500'000), true, 7);
}

// Full 48-byte messages: every field must arrive intact (no torn or stale
// slot), through a tiny ring so slots are reused constantly.
TYPED_TEST(QueueStress, MessagePayloadIntegrity) {
    using Q = typename TypeParam::template of<Msg, 8>;
    const uint64_t n = scaled(2'000'000);
    auto q = std::make_unique<Q>();
    uint64_t bad = 0;
    std::thread consumer([&] {
        Msg m;
        for (uint64_t i = 0; i < n; ++i) {
            q->pop(m);
            bad += !(m.ref == i && m.new_ref == ~i && m.stamp == i * 3 && m.timestamp == i * 7 &&
                     m.price == static_cast<uint32_t>(i) && m.shares == static_cast<uint32_t>(i ^ 0x5555) &&
                     m.locate == static_cast<uint16_t>(i));
        }
    });
    Msg m{};
    for (uint64_t i = 0; i < n; ++i) {
        m.ref = i;
        m.new_ref = ~i;
        m.stamp = i * 3;
        m.timestamp = i * 7;
        m.price = static_cast<uint32_t>(i);
        m.shares = static_cast<uint32_t>(i ^ 0x5555);
        m.locate = static_cast<uint16_t>(i);
        q->push(m);
    }
    q->flush();
    consumer.join();
    EXPECT_EQ(bad, 0u);
}

// Single-threaded full/empty behaviour: a queue of capacity C accepts at
// least C-1 items (some designs keep one slot empty), returns them in order,
// and reports full and empty correctly.
TYPED_TEST(QueueStress, FullAndEmpty) {
    using Q = typename TypeParam::template of<uint64_t, 16>;
    auto q = std::make_unique<Q>();
    uint64_t v;
    EXPECT_FALSE(q->try_pop(v));
    uint64_t pushed = 0;
    while (pushed < 64 && q->try_push(pushed)) ++pushed;
    EXPECT_GE(pushed, 15u);
    EXPECT_LE(pushed, 16u);
    q->flush();
    for (uint64_t i = 0; i < pushed; ++i) {
        ASSERT_TRUE(q->try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q->try_pop(v));
}

// Book differential: the pipeline built on this queue must produce the
// golden model's book hash at every checkpoint (every 4096 messages), on a
// normal ring and on a 4-slot ring that wraps on almost every message.
TYPED_TEST(QueueStress, BookDifferentialEveryCheckpoint) {
    static const auto feed = synth::synthetic_feed(300'000, 99, 32);
    const uint8_t* b = feed.data();
    const uint8_t* e = b + feed.size();
    auto g = std::make_unique<GoldenBook<true>>();
    const RunResult rg = run_single(b, e, *g, true, 4096);
    auto check = [&]<class Q>(double rate) {
        auto book = std::make_unique<FastBook<true>>(1 << 16);
        PipelineOptions opt;
        opt.verify = true;
        opt.checkpoint_every = 4096;
        opt.rate = rate;
        opt.sample_every = 4;
        const RunResult rp = run_pipeline<Q>(b, e, *book, opt);
        ASSERT_EQ(rp.checkpoints.size(), rg.checkpoints.size());
        for (std::size_t i = 0; i < rg.checkpoints.size(); ++i)
            ASSERT_EQ(rp.checkpoints[i], rg.checkpoints[i]) << "first divergence at checkpoint " << i;
        EXPECT_EQ(rp.digest, rg.digest);
        std::string err;
        EXPECT_TRUE(books_equal(*g, *book, &err)) << err;
    };
    check.template operator()<typename TypeParam::template of<Msg, 1024>>(0);
    check.template operator()<typename TypeParam::template of<Msg, 4>>(0);
    check.template operator()<typename TypeParam::template of<Msg, 4>>(3'000'000);  // paced
}
