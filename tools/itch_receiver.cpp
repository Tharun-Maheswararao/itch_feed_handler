// itch_receiver --line-a HOST:PORT --line-b HOST:PORT [options]
//
// Receives MoldUDP64 on two lines, arbitrates, rebuilds the order-level book
// and verifies it against the golden hash file (Stages C2-C4).
//
//   --golden FILE         checkpoint hashes from tools/golden_hashes; every checkpoint must match
//   --capture FILE        write the delivered messages in ITCH file framing (cmp with the source)
//   --interface IP        multicast interface;  --rcvbuf BYTES (default 8 MB)
//   --hw-timestamps       ask the NIC for hardware receive stamps (else kernel software stamps)
//   --cpu-rx N --cpu-book N
//   --buffer-packets N    reorder buffer slots (default 4096)
//   --start-timeout-ms N  --idle-timeout-ms N
//   --sample-shift N      time 1 in 2^N book messages (default 4)
//   --json FILE           also write the results as one JSON object (bench scripts)
//   --no-queue-hash       at checkpoints check the level digest only (latency runs); the
//                         final book's queue order is always checked
//
// Recovery (Stage C3):
//   --retransmit HOST:PORT  --snapshot HOST:PORT
//   --gap-timeout-us N      wait this long for the other line before re-requesting (default 500)
//   --request-timeout-us N  first retry (default 2000, doubling)   --max-retries N (default 6)
//   --snapshot-threshold N  a gap wider than N messages goes straight to a snapshot
//   --max-request N         messages per re-request (default 2048)
//
// Kernel bypass (Stage C4, Linux builds with the option on):
//   --transport udp|xdp|dpdk  receive path (default udp: kernel sockets)
//   --xdp-if IF [--xdp-queue N] [--xdp-native] [--xdp-zero-copy]   (-DFH_ENABLE_XDP=ON)
//   --dpdk-eal "ARGS" [--dpdk-port N]                              (-DFH_ENABLE_DPDK=ON)
//   With xdp and dpdk the line endpoints only give each line's UDP port
//   (and multicast group, joined so the NIC accepts it).
//
// Exit status 0 only if the run completed and every check passed.
#include <csignal>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

#include "fh/net/receiver.hpp"
#include "fh/net/udp_rx.hpp"
#include "fh/stats/alloc_counter.hpp"
#if defined(FH_HAVE_XDP)
#include <arpa/inet.h>

#include "fh/net/xdp_rx.hpp"
#endif
#if defined(FH_HAVE_DPDK)
#include "fh/net/dpdk_rx.hpp"
#endif

using namespace fh;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
    net::UdpRxConfig udp;
    net::ReceiverConfig cfg;
    std::string golden_path, json_path, transport = "udp";
    struct {  // kernel-bypass options, used only by builds that have the path
        std::string xdp_if, dpdk_eal;
        int xdp_queue = 0, dpdk_port = 0;
        bool xdp_native = false, xdp_zc = false;
    } bp;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--line-a" || a == "--line-b") {
                const std::string v = val();
                udp.line[a == "--line-a" ? 0 : 1] = v == "off" ? "" : v;
            } else if (a == "--golden") golden_path = val();
            else if (a == "--capture") cfg.capture_path = val();
            else if (a == "--interface") udp.socket.interface = val();
            else if (a == "--rcvbuf") udp.socket.buffer_bytes = std::stoi(val());
            else if (a == "--hw-timestamps") udp.hardware_timestamps = true;
            else if (a == "--cpu-rx") cfg.cpu_rx = std::stoi(val());
            else if (a == "--cpu-book") cfg.cpu_book = std::stoi(val());
            else if (a == "--buffer-packets") cfg.arb.buffer_packets = static_cast<uint32_t>(std::stoul(val()));
            else if (a == "--start-timeout-ms") cfg.start_timeout = std::chrono::milliseconds(std::stol(val()));
            else if (a == "--idle-timeout-ms") cfg.idle_timeout = std::chrono::milliseconds(std::stol(val()));
            else if (a == "--sample-shift") cfg.sample_shift = static_cast<uint32_t>(std::stoul(val()));
            else if (a == "--no-queue-hash") cfg.verify_queue_order = false;
            else if (a == "--retransmit") cfg.recovery.retransmit = val();
            else if (a == "--snapshot") cfg.recovery.snapshot = val();
            else if (a == "--gap-timeout-us") cfg.recovery.gap_timeout = std::chrono::microseconds(std::stol(val()));
            else if (a == "--request-timeout-us")
                cfg.recovery.request_timeout = std::chrono::microseconds(std::stol(val()));
            else if (a == "--max-retries") cfg.recovery.max_retries = std::stoi(val());
            else if (a == "--snapshot-threshold") cfg.recovery.snapshot_threshold = std::stoull(val());
            else if (a == "--max-request") cfg.recovery.max_request = static_cast<uint32_t>(std::stoul(val()));
            else if (a == "--transport") transport = val();
            else if (a == "--json") json_path = val();
            else if (a == "--xdp-if") bp.xdp_if = val();
            else if (a == "--xdp-queue") bp.xdp_queue = std::stoi(val());
            else if (a == "--xdp-native") bp.xdp_native = true;
            else if (a == "--xdp-zero-copy") bp.xdp_zc = true;
            else if (a == "--dpdk-eal") bp.dpdk_eal = val();
            else if (a == "--dpdk-port") bp.dpdk_port = std::stoi(val());
            else {
                std::fprintf(stderr, "usage: itch_receiver --line-a HOST:PORT --line-b HOST:PORT [--golden FILE] "
                                     "[--capture FILE] [--interface IP] [--rcvbuf B] [--hw-timestamps] [--cpu-rx N] "
                                     "[--cpu-book N] [--buffer-packets N] [--idle-timeout-ms N]\n");
                return 2;
            }
        }
        golden::File gold;
        if (!golden_path.empty()) {
            gold = golden::read(golden_path);
            cfg.golden = &gold;
        }
#if defined(FH_HAVE_ALLOC_COUNTER)
        cfg.alloc_count = &allocation_count;
#endif
        cfg.announce_ready = true;

        // Each line's port (and multicast group) from its endpoint.
        uint16_t port[2] = {0, 0};
        uint32_t group[2] = {0, 0};
        for (int l = 0; l < 2; ++l) {
            if (udp.line[l].empty()) continue;
            const net::Endpoint e = net::parse_endpoint(udp.line[l]);
            port[l] = e.port();
            if (e.multicast()) group[l] = ntohl(e.addr.sin_addr.s_addr);
        }
        (void)group;
        std::unique_ptr<net::RxPath> rx;
        if (transport == "udp") {
            auto u = std::make_unique<net::UdpRx>(udp);
            std::printf("listening: A=%s (rcvbuf %d) B=%s (rcvbuf %d), %s requested\n",
                        udp.line[0].empty() ? "off" : udp.line[0].c_str(), u->receive_buffer(0),
                        udp.line[1].empty() ? "off" : udp.line[1].c_str(), u->receive_buffer(1),
                        net::ts_source_name(u->requested_ts()));
            rx = std::move(u);
        } else if (transport == "xdp") {
#if defined(FH_HAVE_XDP)
            net::XdpRxConfig x;
            x.ifname = bp.xdp_if;
            x.queue = bp.xdp_queue;
            x.native = bp.xdp_native;
            x.zero_copy = bp.xdp_zc;
            for (int l = 0; l < 2; ++l) {
                x.port[l] = port[l];
                x.group[l] = group[l];
            }
            rx = std::make_unique<net::XdpRx>(x);
#else
            throw std::runtime_error("--transport xdp needs a Linux build with -DFH_ENABLE_XDP=ON");
#endif
        } else if (transport == "dpdk") {
#if defined(FH_HAVE_DPDK)
            net::DpdkRxConfig d;
            std::istringstream ss(bp.dpdk_eal);
            for (std::string w; ss >> w;) d.eal_args.push_back(w);
            d.port_id = static_cast<uint16_t>(bp.dpdk_port);
            d.port[0] = port[0];
            d.port[1] = port[1];
            rx = std::make_unique<net::DpdkRx>(d);
#else
            throw std::runtime_error("--transport dpdk needs a Linux build with -DFH_ENABLE_DPDK=ON");
#endif
        } else {
            throw std::runtime_error("unknown transport " + transport + " (udp, xdp or dpdk)");
        }
        if (transport != "udp") std::printf("listening: %s, ports A %u B %u\n", rx->name(), port[0], port[1]);
        std::fflush(stdout);
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        const auto rep = net::run_receiver(*rx, cfg, &g_stop);
        net::print_report(rep, stdout);
        if (!json_path.empty()) {
            std::FILE* j = std::fopen(json_path.c_str(), "w");
            if (!j) throw std::runtime_error("cannot write " + json_path);
            net::write_json(rep, j);
            std::fclose(j);
        }
        return rep.passed() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "itch_receiver: %s\n", e.what());
        return 1;
    }
}
