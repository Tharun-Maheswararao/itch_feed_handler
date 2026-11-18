// Book snapshots for recovery (Stage C3): the state of the order-level book
// after message S, so a receiver that cannot recover a gap (beyond the
// retransmit server's retention, too many failed retries, or a late join)
// loads it and continues from S + 1. This is the Nasdaq GLIMPSE model; here
// it is a plain TCP stream rather than SoupBinTCP.
//
// Wire format, all integers big endian:
//   request   "SNAP" u64 min_seq                     (12 bytes)
//   response  "FHSNAP01" u64 seq u64 book_msgs u64 level_digest
//             u32 n_symbols u32 n_orders              (40 bytes)
//             n_symbols x { u16 locate, char[8] symbol }
//             n_orders  x { u16 locate, u8 side, u32 price, u64 ref, u32 shares }
//
// Orders are sent stock by stock and side by side, levels in price order and
// each level's orders in queue order, so adding them in stream order rebuilds
// every queue exactly.
//
// book_msgs and level_digest are verification extras, not part of GLIMPSE:
// the number of book messages up to S and Version 1's chained level digest
// at S. With them the receiver resumes the per-message digest chain, so
// every golden checkpoint after the snapshot is still checked in full.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "fh/book/book_common.hpp"
#include "fh/parser/endian.hpp"
#include "fh/parser/message.hpp"

namespace fh::recovery {

inline constexpr char kSnapRequest[4] = {'S', 'N', 'A', 'P'};
inline constexpr std::size_t kSnapRequestLen = 12;
inline constexpr char kSnapMagic[8] = {'F', 'H', 'S', 'N', 'A', 'P', '0', '1'};
inline constexpr std::size_t kSnapHeaderLen = 40;
inline constexpr std::size_t kSnapSymbolLen = 10;
inline constexpr std::size_t kSnapOrderLen = 19;

struct SnapshotHeader {
    uint64_t seq = 0;  // last message included; resume at seq + 1
    uint64_t book_msgs = 0;
    uint64_t level_digest = 0;
    uint32_t n_symbols = 0, n_orders = 0;

    std::size_t total_bytes() const {
        return kSnapHeaderLen + n_symbols * kSnapSymbolLen + std::size_t{n_orders} * kSnapOrderLen;
    }
};

inline void write_snapshot_header(uint8_t* p, const SnapshotHeader& h) {
    std::memcpy(p, kSnapMagic, 8);
    store_be64(p + 8, h.seq);
    store_be64(p + 16, h.book_msgs);
    store_be64(p + 24, h.level_digest);
    store_be32(p + 32, h.n_symbols);
    store_be32(p + 36, h.n_orders);
}

inline bool read_snapshot_header(const uint8_t* p, SnapshotHeader& h) {
    if (std::memcmp(p, kSnapMagic, 8) != 0) return false;
    h.seq = load_be64(p + 8);
    h.book_msgs = load_be64(p + 16);
    h.level_digest = load_be64(p + 24);
    h.n_symbols = load_be32(p + 32);
    h.n_orders = load_be32(p + 36);
    return true;
}

inline void write_snapshot_order(uint8_t* p, uint16_t locate, Side side, uint32_t price, uint64_t ref, uint32_t shares) {
    store_be16(p, locate);
    p[2] = static_cast<uint8_t>(side);
    store_be32(p + 3, price);
    store_be64(p + 7, ref);
    store_be32(p + 15, shares);
}

// Serialize a book (FifoBook's query API) as a snapshot after message
// h.seq. h.n_symbols and h.n_orders are filled in. Server side: allocates.
template <class Book>
void write_snapshot(const Book& book, SnapshotHeader h, std::vector<uint8_t>& out) {
    h.n_symbols = h.n_orders = 0;
    out.assign(kSnapHeaderLen, 0);
    for (uint32_t loc = 0; loc < kMaxLocates; ++loc) {
        const std::string s = book.symbol(static_cast<uint16_t>(loc));
        if (s.empty()) continue;
        uint8_t e[kSnapSymbolLen];
        store_be16(e, static_cast<uint16_t>(loc));
        std::memset(e + 2, ' ', 8);
        std::memcpy(e + 2, s.data(), s.size() < 8 ? s.size() : 8);
        out.insert(out.end(), e, e + kSnapSymbolLen);
        ++h.n_symbols;
    }
    for (uint32_t loc = 0; loc < kMaxLocates; ++loc) {
        for (Side side : {Side::Buy, Side::Sell}) {
            book.for_each_order(static_cast<uint16_t>(loc), side, true, [&](uint32_t price, uint64_t ref, uint32_t shares) {
                uint8_t e[kSnapOrderLen];
                write_snapshot_order(e, static_cast<uint16_t>(loc), side, price, ref, shares);
                out.insert(out.end(), e, e + kSnapOrderLen);
                ++h.n_orders;
            });
        }
    }
    write_snapshot_header(out.data(), h);
}

// Load a complete snapshot (header included) into an empty book: the
// directory first, then every order in stream order. No allocation.
template <class Book>
void load_snapshot(Book& book, const uint8_t* data) {
    SnapshotHeader h;
    read_snapshot_header(data, h);
    const uint8_t* p = data + kSnapHeaderLen;
    char sym[8];
    for (uint32_t i = 0; i < h.n_symbols; ++i, p += kSnapSymbolLen) {
        std::memcpy(sym, p + 2, 8);
        book.directory(load_be16(p), sym);
    }
    for (uint32_t i = 0; i < h.n_orders; ++i, p += kSnapOrderLen)
        book.add(load_be64(p + 7), load_be16(p), static_cast<Side>(p[2]), load_be32(p + 3), load_be32(p + 15));
}

}  // namespace fh::recovery
