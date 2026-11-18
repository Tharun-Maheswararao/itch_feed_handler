// Sequence number -> byte offset over a buffer in ITCH file framing, so the
// retransmit server can answer any request without scanning from the start.
//
// Sparse: one offset per kStride messages (4.2 M entries, 34 MB, for the full
// 268.7 M-message day). A lookup jumps to the nearest indexed message and
// walks at most kStride - 1 blocks forward.
//
// Sequence numbers are 1-based, as in MoldUDP64: message 1 is the first block.
#pragma once

#include <cstdint>
#include <vector>

#include "fh/parser/endian.hpp"

namespace fh::mold {

class MessageIndex {
public:
    static constexpr uint64_t kStride = 64;

    MessageIndex(const uint8_t* data, const uint8_t* end) : data_(data), end_(end) {
        const uint8_t* p = data;
        uint64_t n = 0;
        while (end - p >= 2) {
            const uint16_t len = load_be16(p);
            if (static_cast<std::size_t>(end - p) < 2u + len) break;
            if (n % kStride == 0) offsets_.push_back(static_cast<uint64_t>(p - data));
            p += 2 + len;
            ++n;
        }
        messages_ = n;
    }

    uint64_t messages() const { return messages_; }  // last valid sequence number
    const uint8_t* end() const { return end_; }

    // Pointer to the block (its length prefix) of message `seq`, or nullptr
    // if seq is outside [1, messages()].
    const uint8_t* find(uint64_t seq) const {
        if (seq == 0 || seq > messages_) return nullptr;
        const uint64_t i = seq - 1;
        const uint8_t* p = data_ + offsets_[i / kStride];
        for (uint64_t k = i % kStride; k > 0; --k) p += 2 + load_be16(p);
        return p;
    }

private:
    const uint8_t* data_;
    const uint8_t* end_;
    std::vector<uint64_t> offsets_;
    uint64_t messages_ = 0;
};

}  // namespace fh::mold
