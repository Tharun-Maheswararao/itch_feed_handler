// Pieces shared by the order-level books (FifoBook and GoldenFifoBook).
#pragma once

#include <cstdint>
#include <string>

#include "fh/book/book_common.hpp"

namespace fh {

struct QueuePosition {
    uint32_t index = 0;         // orders ahead in the level (0 = first in line)
    uint64_t shares_ahead = 0;  // their total shares
    bool operator==(const QueuePosition&) const = default;
};

// Capacity errors and structure statistics of FifoBook.
//   order_pool_full, level_pool_full, map_full: the book DROPPED data and no
//     longer matches the feed. capacity_ok() is false.
//   window_pool_full: a stock-side got no price window and keeps all its
//     levels in the treap. Still correct, only slower: reported, not an error.
struct FifoCounters {
    uint64_t order_pool_full = 0;
    uint64_t level_pool_full = 0;
    uint64_t map_full = 0;
    uint64_t window_pool_full = 0;
    uint64_t recentres = 0;   // window moves (including first placement)
    uint64_t far_levels = 0;  // levels created outside the window (treap)
    bool capacity_ok() const { return !order_pool_full && !level_pool_full && !map_full; }
};

// Order-level hash. Folded over every stock, both sides, levels in ascending
// price, orders in queue order, so any difference in queue order changes it.
// (Version 1's level hash sums per-level totals and cannot see queue order.)
inline constexpr uint64_t kOrderHashSeed = 0x6A09E667F3BCC908ull;

inline uint64_t order_hash_step(uint64_t h, uint16_t locate, Side side, uint32_t price, uint64_t ref,
                                uint32_t shares) noexcept {
    const uint64_t where = (uint64_t{locate} << 40) | (uint64_t{static_cast<uint8_t>(side)} << 32) | price;
    return mix64(h ^ mix64(where) ^ mix64(ref + 0x9E3779B97F4A7C15ull * shares));
}

// Symbol from a fixed 8-byte field, trimming trailing spaces and NULs.
inline std::string trim_fixed_symbol(const char* p) {
    std::string s(p, 8);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

}  // namespace fh
