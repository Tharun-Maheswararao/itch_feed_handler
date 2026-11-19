#include "fh/recovery/recovery_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "fh/book/book_common.hpp"
#include "fh/net/udp.hpp"
#include "fh/recovery/snapshot.hpp"
#include "fh/util/platform.hpp"

namespace fh::recovery {

using clk = std::chrono::steady_clock;

namespace {

bool read_all(int c, uint8_t* b, std::size_t n) {
    while (n > 0) {
        const ssize_t r = recv(c, b, n, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        b += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

}  // namespace

RecoveryClient::RecoveryClient(const RecoveryConfig& cfg) : cfg_(cfg) {
    if (!cfg_.retransmit.empty()) {
        const net::Endpoint server = net::parse_endpoint(cfg_.retransmit);
        std::memcpy(server_addr_, &server.addr, sizeof(sockaddr_in));
        fd_ = net::open_udp_socket(net::parse_endpoint("0.0.0.0:0"), 8 << 20);
        have_retransmit_ = true;
    }
    if (!cfg_.snapshot.empty()) {
        const net::Endpoint snap = net::parse_endpoint(cfg_.snapshot);
        std::memcpy(snapshot_addr_, &snap.addr, sizeof(sockaddr_in));
        snap_.resize(kSnapHeaderLen + kMaxLocates * kSnapSymbolLen + std::size_t{cfg_.max_orders} * kSnapOrderLen);
    }
}

RecoveryClient::~RecoveryClient() {
    stop();
    if (fd_ >= 0) close(fd_);
}

void RecoveryClient::start() {
    thread_ = std::thread([this] { run(); });
}

void RecoveryClient::stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

RecoveryStats RecoveryClient::stats() const { return st_; }

void RecoveryClient::hand_over(const Recovered& r) {
    while (!recovered_.try_push(r)) {
        if (stop_.load(std::memory_order_relaxed)) return;
        cpu_relax();
    }
}

void RecoveryClient::send_round(const GapRequest& g, uint64_t from) {
    const uint64_t to = std::min(g.to, from + uint64_t{cfg_.inflight} * cfg_.max_request);
    uint8_t req[mold::kHeaderLen];
    for (uint64_t s = from; s < to; s += cfg_.max_request) {
        const auto count = static_cast<uint16_t>(std::min<uint64_t>(cfg_.max_request, to - s));
        mold::write_header(req, g.session, s, count);
        sendto(fd_, req, sizeof req, 0, reinterpret_cast<const sockaddr*>(server_addr_), sizeof(sockaddr_in));
        ++st_.rerequests;
    }
    sent_to_ = to;
}

void RecoveryClient::drain_responses() {
    if (fd_ < 0) return;
    for (;;) {
        const ssize_t n = recv(fd_, scratch_.data, sizeof scratch_.data, 0);
        if (n <= 0) return;
        mold::Header h;
        if (!mold::read_header(scratch_.data, static_cast<std::size_t>(n), h)) continue;
        scratch_.kind = Recovered::Packet;
        scratch_.seq = h.seq;
        scratch_.len = static_cast<uint32_t>(n);
        hand_over(scratch_);
        ++st_.packets;
    }
}

bool RecoveryClient::fetch_snapshot(uint64_t min_seq) {
    if (snap_.empty()) return false;
    struct Flag {  // set for the whole fetch, cleared on every return
        std::atomic<bool>& f;
        explicit Flag(std::atomic<bool>& x) : f(x) { f.store(true, std::memory_order_release); }
        ~Flag() { f.store(false, std::memory_order_release); }
    } flag(fetching_);
    const auto deadline = clk::now() + cfg_.snapshot_timeout;
    // The book thread may still be loading the previous snapshot.
    while (snap_busy_.load(std::memory_order_acquire)) {
        if (stop_.load(std::memory_order_relaxed) || clk::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    for (int attempt = 0; attempt < cfg_.snapshot_attempts && !stop_.load(std::memory_order_relaxed); ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline - clk::now()).count();
        if (left <= 0) break;
        const int c = socket(AF_INET, SOCK_STREAM, 0);
        if (c < 0) continue;
        // The server may replay a long way to reach min_seq before it sends
        // the first byte: allow it the rest of the deadline.
        timeval tv{static_cast<decltype(tv.tv_sec)>(left), 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t req[kSnapRequestLen];
        std::memcpy(req, kSnapRequest, 4);
        store_be64(req + 4, min_seq);
        SnapshotHeader h;
        const bool ok = connect(c, reinterpret_cast<const sockaddr*>(snapshot_addr_), sizeof(sockaddr_in)) == 0 &&
                        send(c, req, sizeof req, 0) == static_cast<ssize_t>(sizeof req) &&
                        read_all(c, snap_.data(), kSnapHeaderLen) && read_snapshot_header(snap_.data(), h) &&
                        h.total_bytes() <= snap_.size() &&
                        read_all(c, snap_.data() + kSnapHeaderLen, h.total_bytes() - kSnapHeaderLen);
        close(c);
        if (!ok) {
            ++st_.snapshot_failures;
            continue;
        }
        ++st_.snapshots;
        st_.snapshot_bytes += h.total_bytes();
        snap_busy_.store(true, std::memory_order_release);
        scratch_.kind = Recovered::Snapshot;
        scratch_.seq = h.seq;
        scratch_.len = 0;
        hand_over(scratch_);
        return true;
    }
    return false;
}

void RecoveryClient::run() {
    bool active = false;
    GapRequest g{};
    int attempt = 0;
    uint64_t mark = 0;
    auto next_send = clk::now();
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!active && requests_.try_pop(g)) {
            active = true;
            attempt = 0;
            mark = 0;
            sent_to_ = 0;
            next_send = clk::now();
            ++st_.gaps;
        }
        if (active) {
            const uint64_t e = expected_.load(std::memory_order_acquire);
            if (e >= g.to) {
                active = false;
                continue;
            }
            if (!have_retransmit_ || g.to - e > cfg_.snapshot_threshold) {
                fetch_snapshot(g.to - 1);
                active = false;
                continue;
            }
            const auto now = clk::now();
            // Next round: on timeout, or at once when the last round is filled.
            if (now >= next_send || (sent_to_ && e >= sent_to_)) {
                if (mark != 0 && e <= mark) {
                    ++st_.retries;
                    if (++attempt > cfg_.max_retries) {
                        ++st_.exhausted;
                        fetch_snapshot(g.to - 1);
                        active = false;
                        continue;
                    }
                } else {
                    attempt = 0;
                }
                send_round(g, e);
                mark = e;
                next_send = now + std::min<clk::duration>(cfg_.request_timeout * (1 << attempt), std::chrono::seconds(1));
            }
        }
        drain_responses();
        if (fd_ >= 0) {
            pollfd pf{fd_, POLLIN, 0};
            ::poll(&pf, 1, active ? 0 : 1);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
}

}  // namespace fh::recovery
