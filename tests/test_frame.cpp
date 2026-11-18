// Ethernet/IPv4/UDP parsing used by the AF_XDP and DPDK receive paths. It
// runs on frames the kernel never checked, so it must reject anything short,
// fragmented or not UDP without reading past the frame.
#include <gtest/gtest.h>

#include <vector>

#include "fh/net/frame.hpp"

using namespace fh;
using namespace fh::net;

namespace {

std::vector<uint8_t> frame(const std::vector<uint8_t>& payload, uint16_t port, bool vlan = false, uint8_t ihl = 5,
                           uint16_t frag = 0, uint8_t proto = 17) {
    std::vector<uint8_t> f(12, 0xAA);  // MACs
    if (vlan) {
        f.push_back(0x81);
        f.push_back(0x00);
        f.push_back(0x00);
        f.push_back(0x05);
    }
    f.push_back(0x08);
    f.push_back(0x00);
    const std::size_t ip = f.size();
    const std::size_t ip_len = ihl * 4u + 8 + payload.size();
    f.resize(ip + ihl * 4u + 8, 0);
    f[ip] = static_cast<uint8_t>(0x40 | ihl);
    store_be16(&f[ip + 2], static_cast<uint16_t>(ip_len));
    store_be16(&f[ip + 6], frag);
    f[ip + 9] = proto;
    store_be32(&f[ip + 16], 0xEF010101);  // 239.1.1.1
    const std::size_t udp = ip + ihl * 4u;
    store_be16(&f[udp + 2], port);
    store_be16(&f[udp + 4], static_cast<uint16_t>(8 + payload.size()));
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

}  // namespace

TEST(Frame, PlainVlanAndOptions) {
    const std::vector<uint8_t> pl{1, 2, 3, 4, 5};
    for (bool vlan : {false, true}) {
        for (uint8_t ihl : {5, 6, 15}) {
            const auto f = frame(pl, 31001, vlan, ihl);
            UdpView u;
            ASSERT_TRUE(parse_udp_frame(f.data(), f.size(), u)) << vlan << " " << int(ihl);
            EXPECT_EQ(u.dst_port, 31001);
            EXPECT_EQ(u.dst_ip, 0xEF010101u);
            ASSERT_EQ(u.len, pl.size());
            EXPECT_EQ(std::vector<uint8_t>(u.payload, u.payload + u.len), pl);
        }
    }
}

TEST(Frame, EthernetPaddingIsIgnored) {
    auto f = frame({9}, 7);
    f.resize(60, 0);  // minimum frame size
    UdpView u;
    ASSERT_TRUE(parse_udp_frame(f.data(), f.size(), u));
    EXPECT_EQ(u.len, 1u);
}

TEST(Frame, RejectsWhatIsNotAWholeUdpDatagram) {
    const std::vector<uint8_t> pl(40, 7);
    UdpView u;
    const auto good = frame(pl, 1);
    for (std::size_t n = 0; n < good.size(); ++n) EXPECT_FALSE(parse_udp_frame(good.data(), n, u)) << n;
    auto tcp = frame(pl, 1, false, 5, 0, 6);
    EXPECT_FALSE(parse_udp_frame(tcp.data(), tcp.size(), u));
    auto more = frame(pl, 1, false, 5, 0x2000);  // more fragments
    EXPECT_FALSE(parse_udp_frame(more.data(), more.size(), u));
    auto later = frame(pl, 1, false, 5, 0x0010);  // fragment offset
    EXPECT_FALSE(parse_udp_frame(later.data(), later.size(), u));
    auto bad_ihl = frame(pl, 1, false, 4);
    EXPECT_FALSE(parse_udp_frame(bad_ihl.data(), bad_ihl.size(), u));
    auto ipv6 = good;
    ipv6[12] = 0x86;
    ipv6[13] = 0xDD;
    EXPECT_FALSE(parse_udp_frame(ipv6.data(), ipv6.size(), u));
    auto long_udp = good;
    store_be16(&long_udp[14 + 20 + 4], 2000);  // UDP length beyond the IP packet
    EXPECT_FALSE(parse_udp_frame(long_udp.data(), long_udp.size(), u));
}
