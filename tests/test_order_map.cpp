#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "fh/book/order_map.hpp"

using namespace fh;

TEST(OrderMap, InsertFindErase) {
    OrderMap m(64);
    bool ins;
    OrderEntry* e = m.insert(42, ins);
    ASSERT_TRUE(ins);
    e->shares = 7;
    EXPECT_EQ(m.find(42)->shares, 7u);
    EXPECT_EQ(m.insert(42, ins), e);
    EXPECT_FALSE(ins);
    EXPECT_EQ(m.find(43), nullptr);
    m.erase(e);
    EXPECT_EQ(m.find(42), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderMap, ReferenceZeroIsSupported) {
    OrderMap m(16);
    bool ins;
    m.insert(0, ins)->shares = 5;
    EXPECT_TRUE(ins);
    ASSERT_NE(m.find(0), nullptr);
    EXPECT_EQ(m.find(0)->shares, 5u);
    EXPECT_EQ(m.size(), 1u);
    m.erase(m.find(0));
    EXPECT_EQ(m.find(0), nullptr);
}

// Random churn against std::unordered_map. A small table forces long probe
// chains and wrap-around, which is where backward-shift deletion can break.
TEST(OrderMap, RandomChurnMatchesStdUnorderedMap) {
    OrderMap m(1024);
    std::unordered_map<uint64_t, uint32_t> ref;
    std::vector<uint64_t> keys;
    std::mt19937_64 rng(7);
    for (int step = 0; step < 400'000; ++step) {
        const auto r = rng() % 100;
        if (r < 55 || keys.empty()) {
            const uint64_t k = (rng() % 5000) + 1;
            bool ins;
            OrderEntry* e = m.insert(k, ins);
            const bool std_ins = ref.emplace(k, static_cast<uint32_t>(step)).second;
            ASSERT_EQ(ins, std_ins);
            if (ins) {
                e->shares = static_cast<uint32_t>(step);
                keys.push_back(k);
            }
        } else {
            const std::size_t i = rng() % keys.size();
            const uint64_t k = keys[i];
            keys[i] = keys.back();
            keys.pop_back();
            OrderEntry* e = m.find(k);
            ASSERT_NE(e, nullptr);
            ASSERT_EQ(e->shares, ref.at(k));
            m.erase(e);
            ref.erase(k);
        }
        if (step % 10'000 == 0) {
            for (auto& [k, v] : ref) {
                OrderEntry* e = m.find(k);
                ASSERT_NE(e, nullptr) << "key " << k << " lost at step " << step;
                ASSERT_EQ(e->shares, v);
            }
        }
    }
    EXPECT_EQ(m.size(), ref.size());
}

TEST(OrderMap, GrowsPastHalfLoadAndKeepsEntries) {
    OrderMap m(16);
    bool ins;
    for (uint64_t k = 1; k <= 1000; ++k) m.insert(k, ins)->price = static_cast<uint32_t>(k * 3);
    EXPECT_GT(m.grows(), 0u);
    EXPECT_GE(m.capacity(), 2000u);
    for (uint64_t k = 1; k <= 1000; ++k) {
        ASSERT_NE(m.find(k), nullptr);
        EXPECT_EQ(m.find(k)->price, k * 3);
    }
}

TEST(OrderMap, SequentialReferencesStayFindable) {
    // ITCH references are roughly sequential: make sure Fibonacci hashing
    // copes with dense ranges and full deletion.
    OrderMap m(1 << 16);
    bool ins;
    for (uint64_t k = 1; k <= 30'000; ++k) m.insert(k * 4, ins);
    for (uint64_t k = 1; k <= 30'000; k += 2) m.erase(m.find(k * 4));
    for (uint64_t k = 1; k <= 30'000; ++k) EXPECT_EQ(m.find(k * 4) != nullptr, k % 2 == 0);
    EXPECT_EQ(m.size(), 15'000u);
}
