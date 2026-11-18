// Ethernet -> IPv4 -> UDP header parsing for the kernel-bypass receive paths
// (AF_XDP and DPDK hand over whole frames, not UDP payloads). Optional
// 802.1Q tag; IPv4 options are skipped by IHL. Fragments are rejected: a
// MoldUDP64 packet always fits in one frame.
#pragma once

#include <cstddef>
#include <cstdint>

#include "fh/parser/endian.hpp"

namespace fh::net {

struct UdpView {
    const uint8_t* payload = nullptr;
    uint32_t len = 0;
    uint16_t dst_port = 0;  // host byte order
    uint32_t dst_ip = 0;    // host byte order
};

inline bool parse_udp_frame(const uint8_t* f, std::size_t n, UdpView& out) noexcept {
    std::size_t off = 14;
    if (n < off) return false;
    uint16_t type = load_be16(f + 12);
    if (type == 0x8100) {  // 802.1Q
        if (n < off + 4) return false;
        type = load_be16(f + 16);
        off += 4;
    }
    if (type != 0x0800 || n < off + 20) return false;
    const uint8_t* ip = f + off;
    if ((ip[0] >> 4) != 4) return false;
    const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0x0F) * 4;
    if (ihl < 20 || ip[9] != 17) return false;               // UDP only
    if ((load_be16(ip + 6) & 0x3FFF) != 0) return false;     // MF flag or fragment offset
    const std::size_t ip_len = load_be16(ip + 2);
    if (ip_len < ihl + 8 || off + ip_len > n) return false;  // frames may be padded, never short
    const uint8_t* udp = ip + ihl;
    const std::size_t udp_len = load_be16(udp + 4);
    if (udp_len < 8 || udp_len > ip_len - ihl) return false;
    out.payload = udp + 8;
    out.len = static_cast<uint32_t>(udp_len - 8);
    out.dst_port = load_be16(udp + 2);
    out.dst_ip = load_be32(ip + 16);
    return true;
}

}  // namespace fh::net
