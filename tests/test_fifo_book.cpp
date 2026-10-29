// Order-level book (Version 2, Stage C0): queue-position semantics on both
// the FifoBook and its golden model, FifoBook's window/treap internals, and
// a message-by-message differential against both golden models.
#include <gtest/gtest.h>

#include <memory>
#include <random>

#include "fh/book/apply.hpp"
#include "fh/book/compare.hpp"
#include "fh/book/fifo_book.hpp"
#include "fh/book/golden_book.hpp"
#include "fh/book/golden_fifo_book.hpp"
#include "fh/parser/parser.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;

namespace {
constexpr uint16_t kLoc = 7;
constexpr uint32_t D = 10000;  // $1.00 in ITCH price units

FifoBookConfig small_config() {
    FifoBookConfig c;
    c.max_orders = 1 << 16;
    c.max_levels = 1 << 15;
    c.order_map_slots = 1 << 17;
    c.max_windows = 64;
    return c;
}
}  // namespace

template <class Book>
class FifoSemantics : public ::testing::Test {
protected:
    std::unique_ptr<Book> make() {
        if constexpr (std::is_constructible_v<Book, FifoBookConfig>)
            return std::make_unique<Book>(small_config());
        else
            return std::make_unique<Book>();
    }
    void SetUp() override { b = make(); }
    QueuePosition pos(uint64_t ref) {
        auto p = b->queue_position(ref);
        EXPECT_TRUE(p.has_value()) << "order " << ref << " missing";
        return p.value_or(QueuePosition{});
    }
    void ok() {
        std::string err;
        EXPECT_TRUE(b->check_invariants(&err)) << err;
    }
    std::unique_ptr<Book> b;
};

using FifoBooks = ::testing::Types<FifoBook<true>, GoldenFifoBook<true>>;
TYPED_TEST_SUITE(FifoSemantics, FifoBooks);

TYPED_TEST(FifoSemantics, ArrivalOrderIsQueueOrder) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Buy, 100 * D, 100);
    b.add(2, kLoc, Side::Buy, 100 * D, 200);
    b.add(3, kLoc, Side::Buy, 100 * D, 300);
    EXPECT_EQ(this->pos(1), (QueuePosition{0, 0}));
    EXPECT_EQ(this->pos(2), (QueuePosition{1, 100}));
    EXPECT_EQ(this->pos(3), (QueuePosition{2, 300}));
    this->ok();
}

TYPED_TEST(FifoSemantics, PartialCancelKeepsPlace) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Sell, 50 * D, 500);
    b.add(2, kLoc, Side::Sell, 50 * D, 100);
    b.cancel(1, 300);  // X: reduce, keep priority
    EXPECT_EQ(this->pos(1), (QueuePosition{0, 0}));
    EXPECT_EQ(this->pos(2), (QueuePosition{1, 200}));
    this->ok();
}

TYPED_TEST(FifoSemantics, ExecuteAtHeadAdvancesTheQueue) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Buy, 10 * D, 100);
    b.add(2, kLoc, Side::Buy, 10 * D, 100);
    b.add(3, kLoc, Side::Buy, 10 * D, 100);
    b.execute(1, 40);  // partial fill at the head: still first
    EXPECT_EQ(this->pos(1), (QueuePosition{0, 0}));
    EXPECT_EQ(this->pos(3), (QueuePosition{2, 160}));
    b.execute(1, 60);  // filled: gone, everyone moves up
    EXPECT_FALSE(b.queue_position(1).has_value());
    EXPECT_EQ(this->pos(2), (QueuePosition{0, 0}));
    EXPECT_EQ(this->pos(3), (QueuePosition{1, 100}));
    this->ok();
}

TYPED_TEST(FifoSemantics, ReplaceLosesPriorityEvenAtSamePrice) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Buy, 10 * D, 100);
    b.add(2, kLoc, Side::Buy, 10 * D, 100);
    b.replace(1, 11, 10 * D, 100);  // U at the same price and size: to the back
    EXPECT_FALSE(b.queue_position(1).has_value());
    EXPECT_EQ(this->pos(2), (QueuePosition{0, 0}));
    EXPECT_EQ(this->pos(11), (QueuePosition{1, 100}));
    this->ok();
}

TYPED_TEST(FifoSemantics, ReplaceThatMovesLevelKeepsSide) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Sell, 20 * D, 100);
    b.add(2, kLoc, Side::Sell, 21 * D, 100);
    b.replace(1, 12, 21 * D, 50);  // joins the 21.00 queue behind order 2
    EXPECT_EQ(b.depth(kLoc, Side::Sell), 1u);
    EXPECT_EQ(this->pos(12), (QueuePosition{1, 100}));
    const auto top = b.top(kLoc, Side::Sell, 5);
    ASSERT_EQ(top.size(), 1u);
    EXPECT_EQ(top[0], (LevelView{21 * D, 2, 150}));
    this->ok();
}

TYPED_TEST(FifoSemantics, DeleteFromTheMiddle) {
    auto& b = *this->b;
    for (uint64_t r = 1; r <= 5; ++r) b.add(r, kLoc, Side::Buy, 10 * D, static_cast<uint32_t>(r * 10));
    b.remove(3);
    EXPECT_EQ(this->pos(4), (QueuePosition{2, 30}));
    b.remove(1);
    b.remove(5);
    EXPECT_EQ(this->pos(2), (QueuePosition{0, 0}));
    EXPECT_EQ(this->pos(4), (QueuePosition{1, 20}));
    this->ok();
}

TYPED_TEST(FifoSemantics, DeleteLastOrderEmptiesBook) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Sell, 10 * D, 100);
    b.remove(1);
    EXPECT_EQ(b.depth(kLoc, Side::Sell), 0u);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_EQ(b.hash(), 0u);
    this->ok();
}

TYPED_TEST(FifoSemantics, DuplicateAddReplacesAndGoesToTail) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Buy, 10 * D, 100);
    b.add(2, kLoc, Side::Buy, 10 * D, 100);
    b.add(1, kLoc, Side::Buy, 10 * D, 70);  // invalid in a real feed; counted
    EXPECT_EQ(b.counters().duplicate_ref, 1u);
    EXPECT_EQ(b.counters().live_orders, 2u);
    EXPECT_EQ(this->pos(1), (QueuePosition{1, 100}));
    this->ok();
}

TYPED_TEST(FifoSemantics, UnknownRefsAndOverfillAreCounted) {
    auto& b = *this->b;
    b.execute(99, 1);
    b.cancel(99, 1);
    b.remove(99);
    b.replace(99, 100, D, 1);
    EXPECT_EQ(b.counters().unknown_ref, 4u);
    b.add(1, kLoc, Side::Buy, D, 10);
    b.execute(1, 15);
    EXPECT_EQ(b.counters().overfill, 1u);
    EXPECT_EQ(b.order_count(), 0u);
    this->ok();
}

TYPED_TEST(FifoSemantics, TopOfBookOrderingBothSides) {
    auto& b = *this->b;
    b.add(1, kLoc, Side::Buy, 99 * D, 10);
    b.add(2, kLoc, Side::Buy, 100 * D, 20);
    b.add(3, kLoc, Side::Buy, 98 * D, 30);
    b.add(4, kLoc, Side::Sell, 102 * D, 40);
    b.add(5, kLoc, Side::Sell, 101 * D, 50);
    const auto bids = b.top(kLoc, Side::Buy, 10);
    const auto asks = b.top(kLoc, Side::Sell, 10);
    ASSERT_EQ(bids.size(), 3u);
    ASSERT_EQ(asks.size(), 2u);
    EXPECT_EQ(bids[0].price, 100 * D);
    EXPECT_EQ(bids[2].price, 98 * D);
    EXPECT_EQ(asks[0].price, 101 * D);
    EXPECT_EQ(b.top(kLoc, Side::Buy, 2).size(), 2u);
    this->ok();
}

// ---- FifoBook internals: window, treap, recentre, capacity -----------------

TEST(FifoBookInternals, FarAndSubCentLevelsLiveInTheTreapButOrderCorrectly) {
    auto b = std::make_unique<FifoBook<true>>(small_config());
    auto g = std::make_unique<GoldenFifoBook<true>>();
    auto both = [&](auto fn) {
        fn(*b);
        fn(*g);
    };
    both([](auto& x) { x.add(1, kLoc, Side::Buy, 100 * D, 10); });         // opens the window
    both([](auto& x) { x.add(2, kLoc, Side::Buy, 20 * D, 10); });          // $80 away: treap
    both([](auto& x) { x.add(3, kLoc, Side::Buy, 100 * D - 50, 10); });    // sub-cent, in range: treap
    both([](auto& x) { x.add(4, kLoc, Side::Buy, 99 * D, 10); });          // window
    both([](auto& x) { x.add(5, kLoc, Side::Sell, 5000 * D, 10); });       // stub quote
    EXPECT_GE(b->fifo_counters().far_levels, 2u);
    EXPECT_EQ(b->top(kLoc, Side::Buy, 10), g->top(kLoc, Side::Buy, 10));
    EXPECT_EQ(b->order_hash(), g->order_hash());
    EXPECT_EQ(b->hash(), g->hash());
    std::string err;
    EXPECT_TRUE(b->check_invariants(&err)) << err;
}

TEST(FifoBookInternals, NewBestOutsideWindowRecentresAndMigrates) {
    auto b = std::make_unique<FifoBook<true>>(small_config());
    auto g = std::make_unique<GoldenFifoBook<true>>();
    auto both = [&](auto fn) {
        fn(*b);
        fn(*g);
    };
    // Bids from $100 down to $97, then the market runs up to $110.
    for (uint64_t i = 0; i < 300; ++i) both([&](auto& x) { x.add(i + 1, kLoc, Side::Buy, 100 * D - 100 * static_cast<uint32_t>(i), 10); });
    const uint64_t before = b->fifo_counters().recentres;
    both([](auto& x) { x.add(1000, kLoc, Side::Buy, 110 * D, 10); });  // new best, $10 above: recentre
    EXPECT_GT(b->fifo_counters().recentres, before);
    EXPECT_EQ(b->top(kLoc, Side::Buy, 400), g->top(kLoc, Side::Buy, 400));
    EXPECT_EQ(b->order_hash(), g->order_hash());
    // Remove the new best: old levels must still all be there and ordered.
    both([](auto& x) { x.remove(1000); });
    EXPECT_EQ(b->top(kLoc, Side::Buy, 400), g->top(kLoc, Side::Buy, 400));
    std::string err;
    EXPECT_TRUE(b->check_invariants(&err)) << err;
}

TEST(FifoBookInternals, CapacityExhaustionIsCountedNotGrown) {
    FifoBookConfig c = small_config();
    c.max_orders = 4;
    auto b = std::make_unique<FifoBook<true>>(c);
    for (uint64_t r = 1; r <= 6; ++r) b->add(r, kLoc, Side::Buy, 10 * D, 1);
    EXPECT_EQ(b->order_count(), 4u);
    EXPECT_EQ(b->fifo_counters().order_pool_full, 2u);
    EXPECT_FALSE(b->fifo_counters().capacity_ok());
    std::string err;
    EXPECT_TRUE(b->check_invariants(&err)) << err;
}

// ---- Differential on a wide synthetic feed --------------------------------

TEST(FifoBookDifferential, MatchesBothGoldenModelsOnEveryMessage) {
    const auto feed = synth::synthetic_feed(400'000, 2026, 20, /*wide=*/true);
    auto fifo = std::make_unique<FifoBook<true>>(small_config());
    auto ref = std::make_unique<GoldenFifoBook<true>>();
    auto v1 = std::make_unique<GoldenBook<true>>();
    std::mt19937_64 rng(5);
    uint64_t n = 0, level_mismatch = 0, order_mismatch = 0, qpos_checked = 0;
    std::string err;
    parse_buffer(feed.data(), feed.data() + feed.size(), [&](const Msg& m) {
        apply(*fifo, m);
        apply(*ref, m);
        apply(*v1, m);
        ++n;
        if (fifo->hash() != ref->hash() || fifo->hash() != v1->hash()) ++level_mismatch;
        if (n % 4096 == 0) {
            if (fifo->order_hash() != ref->order_hash()) ++order_mismatch;
            ASSERT_TRUE(fifo->check_invariants(&err)) << "after " << n << ": " << err;
            if (m.type == MsgType::AddOrder || m.type == MsgType::AddOrderMpid) {
                ++qpos_checked;
                ASSERT_EQ(fifo->queue_position(m.ref), ref->queue_position(m.ref));
            }
        }
    });
    EXPECT_EQ(level_mismatch, 0u) << "aggregated view diverged from a golden model";
    EXPECT_EQ(order_mismatch, 0u) << "queue order diverged from the order-level golden model";
    EXPECT_EQ(fifo->order_hash(), ref->order_hash());
    EXPECT_TRUE(books_equal(*v1, *fifo, &err)) << err;
    EXPECT_TRUE(fifo->fifo_counters().capacity_ok());
    EXPECT_GT(fifo->fifo_counters().far_levels, 0u) << "wide feed should exercise the treap";
    EXPECT_GT(fifo->fifo_counters().recentres, 20u) << "wide feed should move windows";
}
