// itch_publisher FILE [options]
//
// Replays an ITCH file as MoldUDP64 over UDP on two lines (Stage C1).
//
//   --line-a HOST:PORT  --line-b HOST:PORT   destinations ("" or "off" disables a line);
//                                            a multicast group works the same as a unicast address
//   --session NAME        MoldUDP64 session (10 characters)
//   --payload BYTES       UDP payload limit, header included (default 1400)
//   --rate MSGS_PER_SEC   fixed message rate
//   --speedup X           pace by ITCH timestamps divided by X
//   --heartbeat-ms N      idle heartbeat interval (default 1000)
//   --interface IP        multicast interface; --ttl N multicast TTL; --sndbuf BYTES
//
// Loss injection (seeded; the same seed drops the same packets):
//   --seed N  --drop-a P  --drop-b P  --drop-both P  --burst-rate P --burst-len N
//   --duplicate P  --reorder P  --drop-range FIRST-LAST[:a|b|ab]  --kill-a PACKET  --kill-b PACKET
//   --log FILE            every non-send action: packet seq count line action
//
// Recovery services (Stage C3), on the same host as the publisher:
//   --retransmit-listen HOST:PORT   answer MoldUDP64 re-requests (UDP)
//   --retention N                   only the newest N messages can be re-sent
//   --max-request N                 messages per request (default 2048)
//   --snapshot-listen HOST:PORT     serve order-level book snapshots (TCP)
//   --snapshot-follow               keep the snapshot replica at the live edge (one busy thread),
//                                   so snapshots need no replay
//   --linger-ms N                   keep serving after the end of session (default 3000);
//                                   SIGINT or SIGTERM ends the linger early
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "fh/mold/message_index.hpp"
#include "fh/mold/publisher.hpp"
#include "fh/parser/mapped_file.hpp"
#include "fh/recovery/servers.hpp"

using namespace fh;

namespace {

mold::Publisher* g_pub = nullptr;
std::atomic<bool> g_stop{false};
void on_signal(int) {
    g_stop = true;
    if (g_pub) g_pub->stop();
}

int usage() {
    std::fprintf(stderr,
                 "usage: itch_publisher FILE --line-a HOST:PORT --line-b HOST:PORT [--rate R | --speedup X]\n"
                 "       [--session S] [--payload N] [--heartbeat-ms N] [--interface IP] [--ttl N] [--sndbuf B]\n"
                 "       [--seed N] [--drop-a P] [--drop-b P] [--drop-both P] [--burst-rate P --burst-len N]\n"
                 "       [--duplicate P] [--reorder P] [--drop-range F-L[:a|b|ab]] [--kill-a K] [--kill-b K] [--log FILE]\n");
    return 2;
}

mold::LossConfig::Range parse_range(const std::string& s) {
    mold::LossConfig::Range r{0, 0, 3};
    const auto dash = s.find('-'), colon = s.find(':');
    r.first = std::stoull(s.substr(0, dash));
    r.last = std::stoull(s.substr(dash + 1, colon == std::string::npos ? std::string::npos : colon - dash - 1));
    if (colon != std::string::npos) {
        const std::string l = s.substr(colon + 1);
        r.lines = (l.find('a') != std::string::npos ? 1 : 0) | (l.find('b') != std::string::npos ? 2 : 0);
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    mold::PublisherConfig cfg;
    std::string log_path, retransmit_listen, snapshot_listen;
    recovery::RetransmitConfig rt;
    long linger_ms = 3000;
    bool snapshot_follow = false;
    try {
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--line-a" || a == "--line-b") {
                std::string v = val();
                cfg.line[a == "--line-a" ? 0 : 1] = v == "off" ? "" : v;
            } else if (a == "--session") cfg.session = mold::Session::from(val().c_str());
            else if (a == "--payload") cfg.payload = std::stoul(val());
            else if (a == "--rate") cfg.rate = std::stod(val());
            else if (a == "--speedup") cfg.speedup = std::stod(val());
            else if (a == "--heartbeat-ms") cfg.heartbeat = std::chrono::milliseconds(std::stol(val()));
            else if (a == "--interface") cfg.socket.interface = val();
            else if (a == "--ttl") cfg.socket.multicast_ttl = std::stoi(val());
            else if (a == "--sndbuf") cfg.socket.buffer_bytes = std::stoi(val());
            else if (a == "--seed") cfg.loss.seed = std::stoull(val());
            else if (a == "--drop-a") cfg.loss.drop[0] = std::stod(val());
            else if (a == "--drop-b") cfg.loss.drop[1] = std::stod(val());
            else if (a == "--drop-both") cfg.loss.drop_both = std::stod(val());
            else if (a == "--burst-rate") cfg.loss.burst_rate = std::stod(val());
            else if (a == "--burst-len") cfg.loss.burst_len = static_cast<uint32_t>(std::stoul(val()));
            else if (a == "--duplicate") cfg.loss.duplicate = std::stod(val());
            else if (a == "--reorder") cfg.loss.reorder = std::stod(val());
            else if (a == "--drop-range") cfg.loss.drop_ranges.push_back(parse_range(val()));
            else if (a == "--kill-a") cfg.loss.kill_from[0] = std::stoull(val());
            else if (a == "--kill-b") cfg.loss.kill_from[1] = std::stoull(val());
            else if (a == "--log") log_path = val();
            else if (a == "--retransmit-listen") retransmit_listen = val();
            else if (a == "--retention") rt.retention = std::stoull(val());
            else if (a == "--max-request") rt.max_count = static_cast<uint32_t>(std::stoul(val()));
            else if (a == "--snapshot-listen") snapshot_listen = val();
            else if (a == "--snapshot-follow") snapshot_follow = true;
            else if (a == "--linger-ms") linger_ms = std::stol(val());
            else return usage();
        }

        MappedFile f(argv[1]);
        mold::Publisher pub(f.data(), f.end(), cfg);
        g_pub = &pub;
        std::unique_ptr<mold::MessageIndex> index;
        std::unique_ptr<recovery::RetransmitServer> rts;
        std::unique_ptr<recovery::SnapshotServer> snaps;
        auto next_seq = [&pub] { return pub.next_seq(); };
        if (!retransmit_listen.empty()) {
            index = std::make_unique<mold::MessageIndex>(f.data(), f.end());
            rt.listen = retransmit_listen;
            rt.payload = cfg.payload;
            rts = std::make_unique<recovery::RetransmitServer>(*index, cfg.session, next_seq, rt);
            std::printf("retransmit server on port %u (%llu messages indexed, retention %s, max %u per request)\n",
                        rts->port(), (unsigned long long)index->messages(),
                        rt.retention == UINT64_MAX ? "all" : std::to_string(rt.retention).c_str(), rt.max_count);
        }
        if (!snapshot_listen.empty()) {
            snaps = std::make_unique<recovery::SnapshotServer>(f.data(), f.end(), next_seq, snapshot_listen,
                                                               snapshot_follow);
            std::printf("snapshot server on port %u\n", snaps->port());
        }
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::printf("publishing %s (%zu bytes) on A=%s B=%s, %s\n", argv[1], f.size(),
                    cfg.line[0].empty() ? "off" : cfg.line[0].c_str(), cfg.line[1].empty() ? "off" : cfg.line[1].c_str(),
                    cfg.rate > 0 ? ("rate " + std::to_string(static_cast<long>(cfg.rate)) + " msgs/s").c_str()
                    : cfg.speedup > 0 ? ("ITCH time / " + std::to_string(cfg.speedup)).c_str()
                                      : "full speed");
        std::fflush(stdout);
        const auto st = pub.run();
        g_pub = nullptr;
        // Keep the recovery servers up for receivers still filling gaps. A
        // script that knows when its receiver is done ends this early with
        // SIGINT/SIGTERM.
        const auto linger_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(linger_ms);
        while ((rts || snaps) && !g_stop && std::chrono::steady_clock::now() < linger_end)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::printf("published %llu messages in %llu packets, %.2f s, %.0f msgs/s\n",
                    (unsigned long long)st.messages, (unsigned long long)st.packets, st.seconds, st.msgs_per_sec());
        for (int l = 0; l < 2; ++l) {
            if (cfg.line[l].empty()) continue;
            std::printf("  line %c: sent %llu, dropped %llu, duplicated %llu, delayed %llu\n", "AB"[l],
                        (unsigned long long)st.sent[l], (unsigned long long)st.dropped[l],
                        (unsigned long long)st.duplicated[l], (unsigned long long)st.delayed[l]);
        }
        std::printf("  heartbeats %llu, send errors %llu, loss seed %llu\n", (unsigned long long)st.heartbeats,
                    (unsigned long long)st.send_errors, (unsigned long long)cfg.loss.seed);
        if (rts) {
            rts->stop();
            const auto r = rts->stats();
            std::printf("  retransmit: %llu requests, %llu messages in %llu packets; trimmed %llu, refused (retention) "
                        "%llu, rate limited %llu, bad %llu\n",
                        (unsigned long long)r.requests, (unsigned long long)r.served_messages,
                        (unsigned long long)r.packets, (unsigned long long)r.trimmed,
                        (unsigned long long)r.refused_retention, (unsigned long long)r.rate_limited,
                        (unsigned long long)r.bad);
        }
        if (snaps) {
            snaps->stop();
            const auto s = snaps->stats();
            std::printf("  snapshots: %llu served (%.1f MB, last at seq %llu), %llu failed\n", (unsigned long long)s.served,
                        static_cast<double>(s.bytes) / 1e6, (unsigned long long)s.last_seq, (unsigned long long)s.failed);
        }
        if (!log_path.empty()) {
            std::FILE* out = std::fopen(log_path.c_str(), "w");
            if (!out) throw std::runtime_error("cannot write " + log_path);
            std::fprintf(out, "# packet seq count line action\n");
            for (const auto& e : pub.log())
                std::fprintf(out, "%llu %llu %u %c %s\n", (unsigned long long)e.packet, (unsigned long long)e.seq,
                             e.count, "AB"[e.line], mold::action_name(e.action));
            std::fclose(out);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "itch_publisher: %s\n", e.what());
        return 1;
    }
}
