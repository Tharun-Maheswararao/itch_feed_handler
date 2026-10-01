// Book semantics and edge cases, run identically against the golden model and
// the optimized book.
#include <gtest/gtest.h>

#include "fh/book/compare.hpp"
#include "fh/book/fast_book.hpp"
#include "fh/book/golden_book.hpp"

using namespace fh;

template <class Book>
class BookTest : public ::testing::Test {
protected:
    std::unique_ptr<Book> make() {
        if constexpr (std::is_constructible_v<Book, std::size_t>)
            return std::make_unique<Book>(1024);
        else
            return std::make_unique<Book>();
    }
    void SetUp() override { b = make(); }
    void expect_ok() {
        std::string err;
        EXPECT_TRUE(b->check_invariants(&err)) << err;
    }
    LevelView best(Side s) {
        auto v = b->top(kLoc, s, 1);
        return v.empty() ? LevelView{} : v[0];
    }
    static constexpr uint16_t kLoc = 13;
    std::unique_ptr<Book> b;
};

using Books = ::testing::Types<GoldenBook<true>, FastBook<true>>;
TYPED_TEST_SUITE(BookTest, Books);

TYPED_TEST(BookTest, AddAggregatesByLevel) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Buy, 100'0000, 100);
    b.add(2, this->kLoc, Side::Buy, 100'0000, 200);
    b.add(3, this->kLoc, Side::Buy, 99'0000, 50);
    b.add(4, this->kLoc, Side::Sell, 101'0000, 10);
    b.add(5, this->kLoc, Side::Sell, 102'0000, 20);
    const auto bids = b.top(this->kLoc, Side::Buy, 10);
    ASSERT_EQ(bids.size(), 2u);
    EXPECT_EQ(bids[0], (LevelView{100'0000, 2, 300}));
    EXPECT_EQ(bids[1], (LevelView{99'0000, 1, 50}));
    const auto asks = b.top(this->kLoc, Side::Sell, 10);
    ASSERT_EQ(asks.size(), 2u);
    EXPECT_EQ(asks[0], (LevelView{101'0000, 1, 10}));
    EXPECT_EQ(asks[1], (LevelView{102'0000, 1, 20}));
    EXPECT_EQ(b.counters().live_orders, 5u);
    this->expect_ok();
}

TYPED_TEST(BookTest, PartialExecuteThenCancel) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Sell, 50'0000, 500);
    b.execute(1, 200);
    EXPECT_EQ(this->best(Side::Sell), (LevelView{50'0000, 1, 300}));
    b.cancel(1, 100);
    EXPECT_EQ(this->best(Side::Sell), (LevelView{50'0000, 1, 200}));
    this->expect_ok();
}

TYPED_TEST(BookTest, ExecutionThatEmptiesALevel) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Buy, 10'0000, 100);
    b.add(2, this->kLoc, Side::Buy, 9'9900, 100);
    b.execute(1, 100);  // the whole best bid trades
    EXPECT_EQ(b.depth(this->kLoc, Side::Buy), 1u);
    EXPECT_EQ(this->best(Side::Buy), (LevelView{9'9900, 1, 100}));
    EXPECT_EQ(b.counters().live_orders, 1u);
    this->expect_ok();
}

TYPED_TEST(BookTest, ExecuteWithPriceUsesOrderPriceLevel) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Buy, 10'0000, 100);
    b.execute(1, 30);  // 'C' maps to execute; book reduces the resting level
    EXPECT_EQ(this->best(Side::Buy), (LevelView{10'0000, 1, 70}));
    this->expect_ok();
}

TYPED_TEST(BookTest, DeleteOfLastOrderLeavesEmptyBook) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Sell, 10'0000, 100);
    const uint64_t h = b.hash();
    EXPECT_NE(h, 0u);
    b.remove(1);
    EXPECT_EQ(b.depth(this->kLoc, Side::Sell), 0u);
    EXPECT_EQ(b.counters().live_orders, 0u);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_EQ(b.hash(), 0u) << "hash of an empty book must be zero";
    this->expect_ok();
}

TYPED_TEST(BookTest, ReplaceMovesOrderToNewLevelAndKeepsSide) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Buy, 10'0000, 100);
    b.add(2, this->kLoc, Side::Buy, 10'0000, 50);
    b.add(3, this->kLoc, Side::Sell, 10'0500, 70);
    b.replace(1, 11, 10'0100, 300);  // improve the bid
    auto bids = b.top(this->kLoc, Side::Buy, 10);
    ASSERT_EQ(bids.size(), 2u);
    EXPECT_EQ(bids[0], (LevelView{10'0100, 1, 300}));
    EXPECT_EQ(bids[1], (LevelView{10'0000, 1, 50}));
    b.cancel(11, 100);  // the new reference is live
    EXPECT_EQ(this->best(Side::Buy).shares, 200u);
    b.cancel(1, 10);  // the old reference is gone
    EXPECT_EQ(b.counters().unknown_ref, 1u);
    this->expect_ok();
}

// ITCH 'U' cannot change side (spec 1.4.5), even when the new price is
// through the opposite touch. The order must stay on its original side.
TYPED_TEST(BookTest, ReplacePricedThroughOtherSideStaysOnItsSide) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Buy, 10'0000, 100);
    b.add(2, this->kLoc, Side::Sell, 10'0500, 100);
    b.replace(1, 12, 10'1000, 100);  // bid now priced above the best ask
    EXPECT_EQ(this->best(Side::Buy), (LevelView{10'1000, 1, 100}));
    EXPECT_EQ(this->best(Side::Sell), (LevelView{10'0500, 1, 100}));
    EXPECT_EQ(b.depth(this->kLoc, Side::Buy), 1u);
    this->expect_ok();
}

TYPED_TEST(BookTest, ReplaceAtSamePriceLosesNothing) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Sell, 10'0000, 100);
    b.replace(1, 2, 10'0000, 40);
    EXPECT_EQ(this->best(Side::Sell), (LevelView{10'0000, 1, 40}));
    EXPECT_EQ(b.counters().adds, 1u);
    EXPECT_EQ(b.counters().replaces, 1u);
    this->expect_ok();
}

TYPED_TEST(BookTest, UnknownReferencesAreCountedAndIgnored) {
    auto& b = *this->b;
    b.execute(999, 1);
    b.cancel(999, 1);
    b.remove(999);
    b.replace(999, 1000, 1, 1);
    EXPECT_EQ(b.counters().unknown_ref, 4u);
    EXPECT_EQ(b.order_count(), 0u);
    this->expect_ok();
}

TYPED_TEST(BookTest, OverfillRemovesOrder) {
    auto& b = *this->b;
    b.add(1, this->kLoc, Side::Buy, 10'0000, 100);
    b.execute(1, 150);
    EXPECT_EQ(b.counters().overfill, 1u);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_EQ(b.depth(this->kLoc, Side::Buy), 0u);
    this->expect_ok();
}

TYPED_TEST(BookTest, DeepBookOrderingAndBinarySearchPath) {
    auto& b = *this->b;
    // 200 levels per side, inserted in a scrambled order, then removed from
    // the middle: exercises the binary-search fallback in FastBook.
    for (uint32_t i = 0; i < 200; ++i) {
        const uint32_t k = (i * 37) % 200;
        b.add(1000 + k, this->kLoc, Side::Buy, 50'0000 - k * 100, 10 + k);
        b.add(5000 + k, this->kLoc, Side::Sell, 50'0100 + k * 100, 10 + k);
    }
    for (uint32_t k = 50; k < 150; ++k) {
        b.remove(1000 + k);
        b.remove(5000 + k);
    }
    const auto bids = b.top(this->kLoc, Side::Buy, SIZE_MAX);
    const auto asks = b.top(this->kLoc, Side::Sell, SIZE_MAX);
    ASSERT_EQ(bids.size(), 100u);
    ASSERT_EQ(asks.size(), 100u);
    for (std::size_t i = 1; i < bids.size(); ++i) EXPECT_GT(bids[i - 1].price, bids[i].price);
    for (std::size_t i = 1; i < asks.size(); ++i) EXPECT_LT(asks[i - 1].price, asks[i].price);
    EXPECT_EQ(bids[0].price, 50'0000u);
    EXPECT_EQ(asks[0].price, 50'0100u);
    this->expect_ok();
}

TYPED_TEST(BookTest, StocksAreIndependent) {
    auto& b = *this->b;
    b.add(1, 1, Side::Buy, 10'0000, 100);
    b.add(2, 2, Side::Buy, 10'0000, 100);
    b.remove(1);
    EXPECT_EQ(b.depth(1, Side::Buy), 0u);
    EXPECT_EQ(b.depth(2, Side::Buy), 1u);
    this->expect_ok();
}

TYPED_TEST(BookTest, DirectoryMapsLocateToSymbol) {
    auto& b = *this->b;
    const char sym[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    b.directory(this->kLoc, sym);
    EXPECT_EQ(b.symbol(this->kLoc), "AAPL");
}

// The two implementations agree on the hash for the same sequence.
TEST(BookCrossCheck, SameSequenceSameHash) {
    GoldenBook<true> g;
    FastBook<true> f(1024);
    auto both = [&](auto fn) {
        fn(g);
        fn(f);
        ASSERT_EQ(g.hash(), f.hash());
    };
    both([](auto& b) { b.add(1, 5, Side::Buy, 100, 10); });
    both([](auto& b) { b.add(2, 5, Side::Sell, 200, 10); });
    both([](auto& b) { b.add(3, 5, Side::Buy, 100, 5); });
    both([](auto& b) { b.execute(1, 4); });
    both([](auto& b) { b.replace(2, 4, 150, 7); });
    both([](auto& b) { b.cancel(3, 5); });
    both([](auto& b) { b.remove(1); });
    std::string err;
    EXPECT_TRUE(books_equal(g, f, &err)) << err;
}
