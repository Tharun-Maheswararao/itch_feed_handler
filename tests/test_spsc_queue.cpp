#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <thread>

#include "fh/parser/message.hpp"
#include "fh/queue/spsc_queue.hpp"

using namespace fh;

namespace {
uint64_t items_from_env(uint64_t def) {
    const char* e = std::getenv("FH_SPSC_ITEMS");
    return e ? std::strtoull(e, nullptr, 10) : def;
}

// Producer pushes 0..n-1, consumer checks each arrives once and in order.
template <std::size_t Cap>
void sequence_test(uint64_t n) {
    auto q = std::make_unique<SpscQueue<uint64_t, Cap>>();
    uint64_t errors = 0, received = 0;
    std::thread consumer([&] {
        for (uint64_t expect = 0; expect < n; ++expect) {
            uint64_t v;
            q->pop(v);
            errors += (v != expect);
            ++received;
        }
    });
    for (uint64_t i = 0; i < n; ++i) q->push(i);
    consumer.join();
    EXPECT_EQ(received, n);
    EXPECT_EQ(errors, 0u);
    uint64_t v;
    EXPECT_FALSE(q->try_pop(v));
}
}  // namespace

TEST(SpscQueue, EmptyFullAndWrap) {
    SpscQueue<int, 4> q;
    int v = -1;
    EXPECT_FALSE(q.try_pop(v));
    for (int round = 0; round < 10; ++round) {  // wraps the index many times
        for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(round * 10 + i));
        EXPECT_FALSE(q.try_push(99)) << "queue of 4 must report full";
        EXPECT_EQ(q.size_approx(), 4u);
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.try_pop(v));
            EXPECT_EQ(v, round * 10 + i);
        }
        EXPECT_FALSE(q.try_pop(v));
    }
}

TEST(SpscQueue, InterleavedPushPop) {
    SpscQueue<int, 8> q;
    int next_in = 0, next_out = 0, v;
    for (int step = 0; step < 1000; ++step) {
        for (int k = 0; k < step % 7; ++k)
            if (q.try_push(next_in)) ++next_in;
        for (int k = 0; k < step % 5; ++k)
            if (q.try_pop(v)) EXPECT_EQ(v, next_out++);
    }
    while (q.try_pop(v)) EXPECT_EQ(v, next_out++);
    EXPECT_EQ(next_in, next_out);
}

// Tiny ring: producer and consumer constantly hit full/empty, which exercises
// the cached-index refresh paths.
TEST(SpscQueue, SequenceTinyCapacity) { sequence_test<2>(items_from_env(20'000'000) / 10); }
TEST(SpscQueue, SequenceSmallCapacity) { sequence_test<64>(items_from_env(20'000'000) / 4); }
// Default 20M items; set FH_SPSC_ITEMS=2000000000 for the billions run.
TEST(SpscQueue, SequenceLargeCapacity) { sequence_test<16384>(items_from_env(20'000'000)); }

// Full 48-byte messages: every field must arrive intact (no torn slots).
TEST(SpscQueue, MessagePayloadIntegrity) {
    constexpr uint64_t n = 2'000'000;
    auto q = std::make_unique<SpscQueue<Msg, 256>>();
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
    consumer.join();
    EXPECT_EQ(bad, 0u);
}

TEST(SpscQueue, IndicesOnSeparateCacheLines) {
    // The queue object must be at least four cache lines (head, cached tail,
    // tail, cached head) plus the slot pointer line.
    EXPECT_GE(sizeof(SpscQueue<uint64_t, 1024>), 5 * kCacheLine);
    EXPECT_EQ(alignof(SpscQueue<uint64_t, 1024>), kCacheLine);
}
