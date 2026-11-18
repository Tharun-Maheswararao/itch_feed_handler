#include "fh/net/dpdk_rx.hpp"

#include <pthread.h>
#include <sched.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "fh/net/frame.hpp"
#include "fh/net/udp.hpp"

namespace fh::net {

namespace {
constexpr int kBurst = 64;
[[noreturn]] void fail(const std::string& what, int rc) {
    throw std::runtime_error("DPDK: " + what + " (" + std::to_string(rc) + ")");
}
}  // namespace

struct DpdkRx::Impl {
    DpdkRxConfig cfg;
    rte_mempool* pool = nullptr;
    rte_mbuf* held[kBurst];
    uint16_t nheld = 0;
    std::string name;
    bool started = false;
};

DpdkRx::DpdkRx(const DpdkRxConfig& c) : impl_(std::make_unique<Impl>()) {
    Impl& I = *impl_;
    I.cfg = c;
    std::vector<std::string> args{"itch_receiver"};
    args.insert(args.end(), c.eal_args.begin(), c.eal_args.end());
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    // rte_eal_init pins the calling thread to the main lcore, and threads
    // started later (the book thread) would inherit that one core. The
    // receiver pins its own threads (--cpu-rx, --cpu-book), so put the
    // original affinity back.
    cpu_set_t before;
    const bool saved = pthread_getaffinity_np(pthread_self(), sizeof before, &before) == 0;
    if (int rc = rte_eal_init(static_cast<int>(argv.size()), argv.data()); rc < 0) fail("rte_eal_init", rc);
    if (saved) pthread_setaffinity_np(pthread_self(), sizeof before, &before);
    if (!rte_eth_dev_is_valid_port(c.port_id)) fail("no ethernet port " + std::to_string(c.port_id), -1);

    I.pool = rte_pktmbuf_pool_create("fh_rx", c.mbufs, 256, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                     static_cast<int>(rte_eth_dev_socket_id(c.port_id)));
    if (!I.pool) fail("mbuf pool", -rte_errno);

    rte_eth_conf conf{};
    if (int rc = rte_eth_dev_configure(c.port_id, 1, 1, &conf)) fail("dev configure", rc);
    uint16_t rxd = c.rx_desc, txd = 512;
    rte_eth_dev_adjust_nb_rx_tx_desc(c.port_id, &rxd, &txd);
    const int socket = rte_eth_dev_socket_id(c.port_id);
    if (int rc = rte_eth_rx_queue_setup(c.port_id, 0, rxd, socket, nullptr, I.pool)) fail("rx queue setup", rc);
    if (int rc = rte_eth_tx_queue_setup(c.port_id, 0, txd, socket, nullptr)) fail("tx queue setup", rc);
    if (int rc = rte_eth_dev_start(c.port_id)) fail("dev start", rc);
    I.started = true;
    rte_eth_promiscuous_enable(c.port_id);
    rte_eth_allmulticast_enable(c.port_id);

    rte_eth_dev_info info{};
    rte_eth_dev_info_get(c.port_id, &info);
    I.name = std::string("DPDK (") + (info.driver_name ? info.driver_name : "?") + ", port " +
             std::to_string(c.port_id) + ")";
}

DpdkRx::~DpdkRx() {
    Impl& I = *impl_;
    release(0);
    if (I.started) {
        rte_eth_dev_stop(I.cfg.port_id);
        rte_eth_dev_close(I.cfg.port_id);
    }
    rte_eal_cleanup();
}

const char* DpdkRx::name() const { return impl_->name.c_str(); }

int DpdkRx::poll(RxPacket* out, int max) {
    Impl& I = *impl_;
    if (I.nheld) release(0);
    const uint16_t want = static_cast<uint16_t>(std::min(max, kBurst));
    rte_mbuf* got[kBurst];
    const uint16_t n = rte_eth_rx_burst(I.cfg.port_id, 0, got, want);
    if (n == 0) return 0;
    const uint64_t now = realtime_ns();
    int k = 0;
    for (uint16_t i = 0; i < n; ++i) {
        rte_mbuf* m = got[i];
        UdpView u;
        int line = -1;
        // Feed packets fit one segment; anything chained is not ours.
        if (m->nb_segs == 1 && parse_udp_frame(rte_pktmbuf_mtod(m, const uint8_t*), rte_pktmbuf_data_len(m), u)) {
            if (u.dst_port == I.cfg.port[0]) line = 0;
            else if (u.dst_port == I.cfg.port[1]) line = 1;
        }
        if (line < 0) {
            rte_pktmbuf_free(m);
            continue;
        }
        I.held[I.nheld++] = m;
        out[k++] = RxPacket{u.payload, u.len, static_cast<uint8_t>(line), now, TsSource::UserPoll};
    }
    return k;
}

void DpdkRx::release(int) {
    Impl& I = *impl_;
    for (uint16_t i = 0; i < I.nheld; ++i) rte_pktmbuf_free(I.held[i]);
    I.nheld = 0;
}

uint64_t DpdkRx::drops() const {
    rte_eth_stats s{};
    if (rte_eth_stats_get(impl_->cfg.port_id, &s) != 0) return 0;
    return s.imissed + s.rx_nombuf + s.ierrors;
}

}  // namespace fh::net
