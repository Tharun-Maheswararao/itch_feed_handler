// Zero-allocation proof for the order-level book: after construction,
// replaying a feed through FifoBook performs no operator new at all.
// Linked with fh_alloc_counter, which replaces global new/delete.
#include <gtest/gtest.h>

#include <memory>

#include "fh/book/apply.hpp"
#include "fh/book/fifo_book.hpp"
#include "fh/book/golden_fifo_book.hpp"
#include "fh/parser/parser.hpp"
#include "fh/recovery/snapshot.hpp"
#include "fh/stats/alloc_counter.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;

namespace {
FifoBookConfig config() {
    FifoBookConfig c;
    c.max_orders = 1 << 18;
    c.max_levels = 1 << 17;
    c.order_map_slots = 1 << 19;
    c.max_windows = 128;
    return c;
}
}  // namespace

TEST(FifoAlloc, CounterSeesAllocations) {
    const uint64_t before = allocation_count();
    auto p = std::make_unique<int>(7);
    auto v = std::make_unique<std::vector<int>>(1000);
    EXPECT_GE(allocation_count() - before, 2u) << "the counter must observe operator new";
}

TEST(FifoAlloc, ReplayAllocatesNothingAfterConstruction) {
    const auto feed = synth::synthetic_feed(300'000, 77, 20, /*wide=*/true);
    auto book = std::make_unique<FifoBook<false>>(config());
    uint64_t sink = 0;
    const uint64_t before = allocation_count();
    // The hot path: parse and apply every message, plus queue-position and
    // top-of-book queries, which must not allocate either.
    parse_buffer(feed.data(), feed.data() + feed.size(), [&](const Msg& m) {
        apply(*book, m);
        if (m.type == MsgType::AddOrder) {
            if (auto qp = book->queue_position(m.ref)) sink += qp->shares_ahead;
            book->top_into(m.locate, m.side, 5, [&](const LevelView& l) { sink += l.shares; });
        }
    });
    const uint64_t after = allocation_count();
    EXPECT_EQ(after - before, 0u) << "FifoBook allocated on the hot path";
    EXPECT_TRUE(book->fifo_counters().capacity_ok());
    EXPECT_NE(sink, 0u);
}

TEST(FifoAlloc, GoldenModelDoesAllocate) {
    // Sanity check on the method: the std::map/std::list reference must show
    // allocations, otherwise the counter would prove nothing.
    const auto feed = synth::synthetic_feed(20'000, 3, 8, true);
    auto g = std::make_unique<GoldenFifoBook<false>>();
    const uint64_t before = allocation_count();
    parse_buffer(feed.data(), feed.data() + feed.size(), [&](const Msg& m) { apply(*g, m); });
    EXPECT_GT(allocation_count() - before, 1000u);
}

// Stage C3 snapshot recovery on the receiver: clear() a used book and load a
// snapshot into it with no allocation, and the result is the same book (same
// queues, same level hash), which then tracks the rest of the feed exactly.
TEST(FifoAlloc, ClearAndSnapshotLoadAllocateNothingAndRebuildTheBook) {
    const auto feed = synth::synthetic_feed(200'000, 41, 20, /*wide=*/true);
    std::vector<Msg> msgs;
    parse_buffer(feed.data(), feed.data() + feed.size(), [&](const Msg& m) { msgs.push_back(m); });
    const std::size_t half = msgs.size() / 2;

    auto source = std::make_unique<FifoBook<true>>(config());
    auto target = std::make_unique<FifoBook<true>>(config());
    for (std::size_t i = 0; i < half; ++i) apply(*source, msgs[i]);
    for (std::size_t i = 0; i < half / 3; ++i) apply(*target, msgs[i]);  // some other, older state
    std::vector<uint8_t> snap;
    recovery::write_snapshot(*source, recovery::SnapshotHeader{half, half, 0, 0, 0}, snap);
    ASSERT_GT(source->order_count(), 1000u);

    const uint64_t before = allocation_count();
    target->clear();
    recovery::load_snapshot(*target, snap.data());
    EXPECT_EQ(allocation_count() - before, 0u) << "clear() or load_snapshot() allocated";

    EXPECT_EQ(target->order_count(), source->order_count());
    EXPECT_EQ(target->hash(), source->hash());
    EXPECT_EQ(target->order_hash(), source->order_hash());
    std::string err;
    EXPECT_TRUE(target->check_invariants(&err)) << err;
    for (std::size_t i = half; i < msgs.size(); ++i) {
        apply(*source, msgs[i]);
        apply(*target, msgs[i]);
    }
    EXPECT_EQ(target->hash(), source->hash());
    EXPECT_EQ(target->order_hash(), source->order_hash());
}

TEST(FifoAlloc, ClearedBookEqualsAFreshOne) {
    const auto feed = synth::synthetic_feed(50'000, 9, 10, true);
    auto used = std::make_unique<FifoBook<true>>(config());
    auto fresh = std::make_unique<FifoBook<true>>(config());
    parse_buffer(feed.data(), feed.data() + feed.size(), [&](const Msg& m) { apply(*used, m); });
    used->clear();
    EXPECT_EQ(used->order_count(), 0u);
    EXPECT_EQ(used->hash(), 0u);
    EXPECT_EQ(used->order_hash(), fresh->order_hash());
    parse_buffer(feed.data(), feed.data() + feed.size(), [&](const Msg& m) {
        apply(*used, m);
        apply(*fresh, m);
    });
    EXPECT_EQ(used->order_hash(), fresh->order_hash());
    EXPECT_EQ(used->hash(), fresh->hash());
}

