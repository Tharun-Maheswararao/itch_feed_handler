// AF_XDP receive path (Stage C4, Linux, -DFH_ENABLE_XDP=ON).
//
// The NIC stays with the kernel. A small XDP program (bpf/xdp_feed.bpf.c)
// redirects UDP packets for the two line ports straight into a UMEM region
// this process maps; everything else still reaches the kernel stack. The
// receiver busy-polls the AF_XDP receive ring, strips the Ethernet, IP and
// UDP headers and hands the payload to the same arbiter as the socket path.
//
// Mode: zero-copy if asked for and the driver supports it, else copy mode;
// native XDP if asked for, else generic (SKB) XDP, which works on any driver
// including veth. The mode that actually ran is in name().
//
// Timestamps: user-space poll stamps (read right after the descriptors are
// found), labelled as such. Hardware stamps through XDP metadata need driver
// support that EC2's ENA does not have.
#pragma once

#include <memory>
#include <string>

#include "fh/net/rx_path.hpp"

namespace fh::net {

struct XdpRxConfig {
    std::string ifname;
    int queue = 0;
    uint16_t port[2] = {0, 0};  // UDP destination port per line (0 = line unused)
    uint32_t group[2] = {0, 0};  // multicast group per line, host order (joined so the NIC accepts it)
    bool native = false;         // native (driver) XDP instead of generic
    bool zero_copy = false;      // try zero-copy first
    // 32 K frames (64 MB UMEM) and a 16 K-entry ring hold about as much as
    // the socket path's 64 MB receive buffer. With 8 K / 4 K the ring
    // overflowed whenever the book thread paused to walk the queues for
    // verification (1.4 M drops in one EC2 run, all recovered).
    uint32_t frames = 32768;
    uint32_t frame_size = 2048;
    uint32_t ring = 16384;       // RX ring entries
    std::string bpf_object;      // compiled xdp_feed.bpf.o (default: the one CMake built)
};

class XdpRx final : public RxPath {
public:
    explicit XdpRx(const XdpRxConfig& c);
    ~XdpRx() override;

    int poll(RxPacket* out, int max) override;
    void release(int count) override;
    const char* name() const override;
    uint64_t drops() const override;

    uint64_t redirected() const;  // packets the XDP program sent to the socket
    uint64_t non_feed() const;    // frames on the socket that were not feed packets

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fh::net
