// DPDK receive path (Stage C4, time-boxed; -DFH_ENABLE_DPDK=ON).
//
// DPDK takes the NIC away from the kernel entirely: a poll-mode driver
// (ENA's on EC2, bound to vfio-pci) fills mbufs that rte_eth_rx_burst
// returns in a busy loop, with no system call and no kernel stack at all.
// On EC2 that means a second network interface dedicated to the feed; the
// first keeps SSH and the GitHub runner.
//
// The same frame parsing and arbiter as AF_XDP. Packets that are not for the
// two line ports are freed at once (ARP and the like are lost: the port is
// the feed's alone).
//
// Timestamps: user-space poll stamps after each burst, labelled as such.
//
// For a functional test without a spare NIC, DPDK's af_packet virtual
// device can read a Linux interface (for example the veth in
// bench/netns_feed.sh): EAL arguments --vdev=net_af_packet0,iface=vrx
// --no-huge --no-pci.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "fh/net/rx_path.hpp"

namespace fh::net {

struct DpdkRxConfig {
    std::vector<std::string> eal_args;  // passed to rte_eal_init (argv[0] added)
    uint16_t port_id = 0;
    uint16_t port[2] = {0, 0};  // UDP destination port per line
    uint32_t mbufs = 16383;
    uint16_t rx_desc = 2048;
};

class DpdkRx final : public RxPath {
public:
    explicit DpdkRx(const DpdkRxConfig& c);
    ~DpdkRx() override;

    int poll(RxPacket* out, int max) override;
    void release(int count) override;
    const char* name() const override;
    uint64_t drops() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fh::net
