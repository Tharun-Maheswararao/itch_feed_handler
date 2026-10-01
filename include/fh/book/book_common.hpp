// Pieces shared by the golden and the optimized book.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "fh/parser/message.hpp"

namespace fh {

inline constexpr std::size_t kMaxLocates = 65536;  // stock locate is a uint16

struct LevelView {
    uint32_t price = 0;
    uint32_t orders = 0;
    uint64_t shares = 0;
    bool operator==(const LevelView&) const = default;
};

// Counters both books keep identically, so they double as a cross-check.
struct BookCounters {
    uint64_t adds = 0, executes = 0, cancels = 0, deletes = 0, replaces = 0, directory = 0;
    uint64_t unknown_ref = 0;     // modify for an order we never saw
    uint64_t overfill = 0;        // execute/cancel larger than the open shares
    uint64_t duplicate_ref = 0;   // add whose reference is already live
    uint64_t live_orders = 0;
    uint64_t peak_live_orders = 0;
    bool operator==(const BookCounters&) const = default;
};

// splitmix64 finalizer: a cheap, well-mixed 64-bit hash.
inline constexpr uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Hash contribution of one non-empty price level. The book hash is the
// wrapping sum of these over every level, so it is order independent and can
// be updated in O(1): subtract the old contribution, add the new one.
inline constexpr uint64_t level_hash(uint16_t locate, Side side, uint32_t price, uint64_t shares,
                                     uint32_t orders) noexcept {
    const uint64_t key = (uint64_t{locate} << 40) | (uint64_t{static_cast<uint8_t>(side)} << 32) | price;
    return mix64(mix64(key) ^ (shares * 0x2545F4914F6CDD1Dull) ^ (uint64_t{orders} << 48));
}

inline std::string trim_symbol(const char (&sym)[8]) {
    std::string s(sym, 8);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

}  // namespace fh
