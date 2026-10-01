// Full-state comparison of two books (any two types with the book API).
#pragma once

#include <cstdint>
#include <string>

#include "fh/book/book_common.hpp"

namespace fh {

template <class A, class B>
bool books_equal(const A& a, const B& b, std::string* err = nullptr) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    if (a.hash() != b.hash()) return fail("book hash differs");
    if (!(a.counters() == b.counters())) return fail("book counters differ");
    if (a.order_count() != b.order_count()) return fail("live order count differs");
    for (std::size_t i = 0; i < kMaxLocates; ++i) {
        const auto loc = static_cast<uint16_t>(i);
        for (Side s : {Side::Buy, Side::Sell}) {
            if (a.depth(loc, s) != b.depth(loc, s) || a.top(loc, s, SIZE_MAX) != b.top(loc, s, SIZE_MAX))
                return fail("levels differ for locate " + std::to_string(i) + " (" + a.symbol(loc) + ") side " +
                            static_cast<char>(s));
        }
        if (a.symbol(loc) != b.symbol(loc)) return fail("symbol differs for locate " + std::to_string(i));
    }
    return true;
}

}  // namespace fh
