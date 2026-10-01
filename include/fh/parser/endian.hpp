// Big-endian field loads. ITCH is network byte order everywhere; x86 and ARM
// are little endian, so every multi-byte field goes through a byte swap.
// memcpy + __builtin_bswap compiles to a single unaligned load + rev/bswap.
#pragma once

#include <cstdint>
#include <cstring>

namespace fh {

inline uint16_t load_be16(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap16(v);
}

inline uint32_t load_be32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);
}

inline uint64_t load_be64(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap64(v);
}

// 6-byte timestamp: 2 high bytes then 4 low bytes. Never reads past p[5],
// so it is safe on the last message of a mapping.
inline uint64_t load_be48(const uint8_t* p) noexcept {
    return (uint64_t{load_be16(p)} << 32) | load_be32(p + 2);
}

// Writers, used by tests and the synthetic feed generator.
inline void store_be16(uint8_t* p, uint16_t v) noexcept {
    v = __builtin_bswap16(v);
    std::memcpy(p, &v, sizeof v);
}
inline void store_be32(uint8_t* p, uint32_t v) noexcept {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, sizeof v);
}
inline void store_be64(uint8_t* p, uint64_t v) noexcept {
    v = __builtin_bswap64(v);
    std::memcpy(p, &v, sizeof v);
}
inline void store_be48(uint8_t* p, uint64_t v) noexcept {
    store_be16(p, static_cast<uint16_t>(v >> 32));
    store_be32(p + 2, static_cast<uint32_t>(v));
}

}  // namespace fh
