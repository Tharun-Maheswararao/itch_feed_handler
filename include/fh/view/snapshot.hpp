// Top-of-book snapshot published by the book thread for the live view.
#pragma once

#include <cstdint>

#include "fh/book/book_common.hpp"

namespace fh {

struct BookSnapshot {
    static constexpr int kDepth = 5;
    uint64_t messages = 0;   // book messages applied so far
    uint64_t timestamp = 0;  // ITCH ns since midnight of the last message
    uint16_t locate = 0;
    uint8_t n_bids = 0, n_asks = 0;
    char symbol[8] = {};
    LevelView bids[kDepth] = {};
    LevelView asks[kDepth] = {};
};

}  // namespace fh
