// MoldUDP64 packet format (Nasdaq MoldUDP64 protocol specification 1.00).
//
//   offset  size  field
//   0       10    session          ASCII, fixed for the whole run
//   10      8     sequence number  big endian, sequence of the first message
//   18      2     message count    0 = heartbeat, 0xFFFF = end of session
//   20      ...   message blocks   2-byte big-endian length, then the message
//
// A message block has exactly the framing of the ITCH file (2-byte length,
// then the message), so a packet's blocks are a contiguous slice of the file
// and the publisher can send them straight from the mapping.
//
// A retransmit request is the same 20-byte header with no blocks: session,
// first missing sequence number, number of messages wanted.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fh/parser/endian.hpp"

namespace fh::mold {

inline constexpr std::size_t kSessionLen = 10;
inline constexpr std::size_t kHeaderLen = 20;
inline constexpr uint16_t kHeartbeat = 0;
inline constexpr uint16_t kEndOfSession = 0xFFFF;
inline constexpr std::size_t kMaxMessages = 0xFFFE;  // a count of 0xFFFF means end of session
inline constexpr std::size_t kDefaultPayload = 1400;  // UDP payload limit: stays under a 1500 MTU
inline constexpr std::size_t kMaxDatagram = 2048;     // receive buffer per packet (> any payload we send)

struct Session {
    char id[kSessionLen];

    static Session from(const char* s) {
        Session r;
        std::memset(r.id, ' ', kSessionLen);
        std::memcpy(r.id, s, std::min(std::strlen(s), kSessionLen));
        return r;
    }
    bool operator==(const Session& o) const { return std::memcmp(id, o.id, kSessionLen) == 0; }
};

struct Header {
    Session session;
    uint64_t seq = 0;    // first message in the packet (heartbeat: next message to be sent)
    uint16_t count = 0;  // messages in the packet, or kHeartbeat / kEndOfSession

    bool heartbeat() const { return count == kHeartbeat; }
    bool end_of_session() const { return count == kEndOfSession; }
    uint64_t messages() const { return heartbeat() || end_of_session() ? 0 : count; }
    uint64_t end() const { return seq + messages(); }  // one past the last message
};

inline void write_header(uint8_t* p, const Session& s, uint64_t seq, uint16_t count) noexcept {
    std::memcpy(p, s.id, kSessionLen);
    store_be64(p + 10, seq);
    store_be16(p + 18, count);
}

inline bool read_header(const uint8_t* p, std::size_t len, Header& h) noexcept {
    if (len < kHeaderLen) return false;
    std::memcpy(h.session.id, p, kSessionLen);
    h.seq = load_be64(p + 10);
    h.count = load_be16(p + 18);
    return true;
}

// Visit the message blocks of a packet: f(seq, msg, len) with `msg` at the
// message's first byte (its type). Returns false if the blocks do not match
// the header's count (a truncated or corrupt packet); messages before the
// fault have already been visited, so callers check validate() first when
// they must not apply a partial packet.
template <class F>
inline bool for_each_message(const uint8_t* pkt, std::size_t len, const Header& h, F&& f) {
    const uint8_t* p = pkt + kHeaderLen;
    const uint8_t* end = pkt + len;
    const uint64_t n = h.messages();
    for (uint64_t i = 0; i < n; ++i) {
        if (end - p < 2) return false;
        const uint16_t mlen = load_be16(p);
        if (end - p - 2 < mlen) return false;
        f(h.seq + i, p + 2, mlen);
        p += 2 + mlen;
    }
    return true;
}

// True if the packet's blocks exactly fill it and match the header's count.
inline bool validate(const uint8_t* pkt, std::size_t len, const Header& h) noexcept {
    const uint8_t* p = pkt + kHeaderLen;
    const uint8_t* end = pkt + len;
    for (uint64_t i = 0, n = h.messages(); i < n; ++i) {
        if (end - p < 2) return false;
        const uint16_t mlen = load_be16(p);
        if (end - p - 2 < mlen) return false;
        p += 2 + mlen;
    }
    return p == end;
}

// One packet's worth of consecutive message blocks taken from a buffer in
// ITCH file framing. `blocks` points into that buffer.
struct Span {
    const uint8_t* blocks = nullptr;
    uint32_t bytes = 0;
    uint16_t count = 0;
};

// Take as many whole blocks from [p, end) as fit in a packet whose UDP
// payload (header included) is at most `payload` bytes, and at most
// `max_count` messages. Advances p. A block that is itself too big for the
// limit still goes out alone, so the packer never stalls.
inline Span take(const uint8_t*& p, const uint8_t* end, std::size_t payload, std::size_t max_count = kMaxMessages) {
    Span s{p, 0, 0};
    const std::size_t room = payload > kHeaderLen ? payload - kHeaderLen : 0;
    while (s.count < max_count && end - p >= 2) {
        const uint16_t mlen = load_be16(p);
        const std::size_t block = 2u + mlen;
        if (static_cast<std::size_t>(end - p) < block) break;  // truncated tail: not a message
        if (s.count > 0 && s.bytes + block > room) break;
        s.bytes += static_cast<uint32_t>(block);
        ++s.count;
        p += block;
    }
    return s;
}

}  // namespace fh::mold
