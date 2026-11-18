// Build a golden hash file in memory from any buffer in ITCH file framing,
// exactly as tools/golden_hashes does for the Nasdaq file. Tests use it to
// verify network replays of synthetic feeds.
#pragma once

#include <memory>
#include <string>

#include "fh/book/apply.hpp"
#include "fh/book/golden_fifo_book.hpp"
#include "fh/parser/parser.hpp"
#include "fh/verify/golden_hashes.hpp"

namespace fh::golden {

inline File build(const uint8_t* data, const uint8_t* end, uint64_t every, const std::string& source = "memory") {
    auto book = std::make_unique<GoldenFifoBook<true>>();
    File out;
    out.source = source;
    out.source_bytes = static_cast<uint64_t>(end - data);
    out.every = every;
    uint64_t n = 0, digest = 0;
    parse_buffer(data, end, [&](const Msg& m) {
        apply(*book, m);
        digest = chain(digest, book->hash());
        if (++n % every == 0) out.checkpoints.push_back({n, digest, book->order_hash()});
    });
    out.final = {n, digest, book->order_hash()};
    return out;
}

}  // namespace fh::golden
