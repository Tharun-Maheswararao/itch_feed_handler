// ITCH 5.0 parser. Walks a pointer over a buffer of length-prefixed messages
// (2-byte big-endian length, then the message), decodes the eight message
// types the book needs and counts every type it sees.
//
// Offsets follow the Nasdaq TotalView-ITCH 5.0 specification, section 1.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fh/parser/endian.hpp"
#include "fh/parser/message.hpp"

namespace fh {

// Minimum on-wire length of each decoded message, from the spec tables.
namespace itch_len {
inline constexpr uint16_t kStockDirectory = 39;
inline constexpr uint16_t kAddOrder = 36;
inline constexpr uint16_t kAddOrderMpid = 40;
inline constexpr uint16_t kOrderExecuted = 31;
inline constexpr uint16_t kOrderExecutedPrice = 36;
inline constexpr uint16_t kOrderCancel = 23;
inline constexpr uint16_t kOrderDelete = 19;
inline constexpr uint16_t kOrderReplace = 35;
}  // namespace itch_len

struct ParseStats {
    std::array<uint64_t, 256> counts{};  // indexed by message type byte
    uint64_t messages = 0;
    uint64_t bytes = 0;
    uint64_t malformed = 0;  // decoded type shorter than the spec length
    bool truncated = false;  // buffer ended mid-message

    uint64_t count(char type) const { return counts[static_cast<uint8_t>(type)]; }
};

// Decode the message body at `m` (type byte first) of length `len` into `out`.
// Returns false for types the book does not need and for malformed messages.
inline bool decode(const uint8_t* m, uint16_t len, Msg& out, ParseStats& st) noexcept {
    const uint8_t type = m[0];
    // Header shared by all types: locate @1, tracking @3, timestamp @5.
    auto header = [&](MsgType t, uint16_t need) {
        if (len < need) [[unlikely]] {
            ++st.malformed;
            return false;
        }
        out.type = t;
        out.locate = load_be16(m + 1);
        out.timestamp = load_be48(m + 5);
        return true;
    };

    switch (type) {
        case 'A':
        case 'F':
            if (!header(type == 'A' ? MsgType::AddOrder : MsgType::AddOrderMpid,
                        type == 'A' ? itch_len::kAddOrder : itch_len::kAddOrderMpid))
                return false;
            out.ref = load_be64(m + 11);
            out.side = static_cast<Side>(m[19]);
            out.shares = load_be32(m + 20);
            out.price = load_be32(m + 32);
            return true;
        case 'E':
            if (!header(MsgType::OrderExecuted, itch_len::kOrderExecuted)) return false;
            out.ref = load_be64(m + 11);
            out.shares = load_be32(m + 19);
            return true;
        case 'C':
            if (!header(MsgType::OrderExecutedPrice, itch_len::kOrderExecutedPrice)) return false;
            out.ref = load_be64(m + 11);
            out.shares = load_be32(m + 19);
            out.price = load_be32(m + 32);
            return true;
        case 'X':
            if (!header(MsgType::OrderCancel, itch_len::kOrderCancel)) return false;
            out.ref = load_be64(m + 11);
            out.shares = load_be32(m + 19);
            return true;
        case 'D':
            if (!header(MsgType::OrderDelete, itch_len::kOrderDelete)) return false;
            out.ref = load_be64(m + 11);
            return true;
        case 'U':
            if (!header(MsgType::OrderReplace, itch_len::kOrderReplace)) return false;
            out.ref = load_be64(m + 11);
            out.new_ref = load_be64(m + 19);
            out.shares = load_be32(m + 27);
            out.price = load_be32(m + 31);
            return true;
        case 'R':
            if (!header(MsgType::StockDirectory, itch_len::kStockDirectory)) return false;
            std::memcpy(out.symbol, m + 11, 8);
            return true;
        default:
            return false;  // every other type: counted by the caller, skipped
    }
}

// Parse every message in [begin, end). `sink(Msg&)` is called for each
// decoded book message; everything else is only counted.
template <class Sink>
ParseStats parse_buffer(const uint8_t* begin, const uint8_t* end, Sink&& sink) {
    ParseStats st;
    const uint8_t* p = begin;
    Msg msg{};
    while (end - p >= 2) {
        const uint16_t len = load_be16(p);
        p += 2;
        if (len == 0) continue;  // zero-length frame: nothing to decode
        if (end - p < len) [[unlikely]] {
            st.truncated = true;
            p -= 2;
            break;
        }
        ++st.counts[p[0]];
        ++st.messages;
        if (decode(p, len, msg, st)) sink(msg);
        p += len;
    }
    st.bytes = static_cast<uint64_t>(p - begin);
    return st;
}

}  // namespace fh
