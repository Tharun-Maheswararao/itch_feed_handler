// The fixed-size message that travels through the ring buffer.
//
// 48 bytes: big enough to carry the ITCH timestamp (used for paced replay and
// the live view) next to the cycle stamp without bit packing. Only the fields
// the book needs are decoded.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace fh {

enum class MsgType : uint8_t {
    None = 0,
    StockDirectory = 'R',
    AddOrder = 'A',
    AddOrderMpid = 'F',
    OrderExecuted = 'E',
    OrderExecutedPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
    EndOfStream = 0xFF,  // sentinel pushed by the parser after the last message
};

enum class Side : uint8_t { Buy = 'B', Sell = 'S' };

struct alignas(16) Msg {
    uint64_t stamp;      // cycle counter, taken right after parsing
    uint64_t timestamp;  // ITCH nanoseconds since midnight
    uint64_t ref;        // order reference (U: original reference)
    union {
        uint64_t new_ref;  // U: new order reference
        char symbol[8];    // R: stock symbol, space padded
    };
    uint32_t price;   // 4 implied decimals (A, F, U; C carries execution price)
    uint32_t shares;  // A/F/U: shares, E/C: executed, X: cancelled
    uint16_t locate;
    MsgType type;
    Side side;  // A/F only
    uint32_t flags;  // pipeline-internal, see kMsgSampled
};

// Msg::flags bit: this message's latency is measured (set by the producer).
inline constexpr uint32_t kMsgSampled = 1;

static_assert(sizeof(Msg) == 48, "Msg must stay 48 bytes");
static_assert(std::is_trivially_copyable_v<Msg>);

}  // namespace fh
