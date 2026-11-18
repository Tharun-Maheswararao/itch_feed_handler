#include "fh/mold/publisher.hpp"

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <thread>

#include "fh/util/platform.hpp"

namespace fh::mold {

using clk = std::chrono::steady_clock;

Publisher::Publisher(const uint8_t* data, const uint8_t* end, PublisherConfig cfg)
    : data_(data), end_(end), cfg_(std::move(cfg)), loss_(cfg_.loss) {
    for (int l = 0; l < 2; ++l)
        if (!cfg_.line[l].empty()) fd_[l] = net::open_udp_sender(net::parse_endpoint(cfg_.line[l]), cfg_.socket);
    if (fd_[0] < 0 && fd_[1] < 0) throw std::runtime_error("publisher: no line configured");
}

Publisher::~Publisher() {
    for (int fd : fd_)
        if (fd >= 0) close(fd);
}

// One datagram: 20-byte header + the blocks, straight from the mapping.
bool Publisher::send_packet(int line, uint64_t seq, uint16_t count, const Span& s) {
    uint8_t hdr[kHeaderLen];
    write_header(hdr, cfg_.session, seq, count);
    iovec iov[2] = {{hdr, kHeaderLen}, {const_cast<uint8_t*>(s.blocks), s.bytes}};
    msghdr m{};
    m.msg_iov = iov;
    m.msg_iovlen = s.bytes ? 2 : 1;
    // A full send buffer (ENOBUFS on macOS, EAGAIN) clears in microseconds:
    // retry briefly. ECONNREFUSED only says nobody listened to the last
    // packet (no receiver yet), which is the line's problem, not ours.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (sendmsg(fd_[line], &m, 0) >= 0) {
            last_send_[line] = clk::now();
            return true;
        }
        if (errno != ENOBUFS && errno != EAGAIN && errno != EINTR) break;
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    ++st_.send_errors;
    return false;
}

void Publisher::send_heartbeats(uint64_t seq, clk::time_point now) {
    for (int l = 0; l < 2; ++l) {
        if (fd_[l] < 0 || loss_.dead(packet_, l) || now - last_send_[l] < cfg_.heartbeat) continue;
        if (send_packet(l, seq, kHeartbeat, Span{})) ++st_.heartbeats;
    }
}

// Wait for a packet's scheduled time. Long waits sleep (and send heartbeats
// on schedule); the last stretch spins so the send time is accurate.
void Publisher::wait_until(clk::time_point t, uint64_t seq) {
    for (;;) {
        const auto now = clk::now();
        if (now >= t || stop_.load(std::memory_order_relaxed)) return;
        send_heartbeats(seq, now);
        const auto left = t - now;
        if (left > std::chrono::milliseconds(2))
            std::this_thread::sleep_for(std::min<clk::duration>(left - std::chrono::milliseconds(1), cfg_.heartbeat / 4));
        else
            cpu_relax();
    }
}

PublisherStats Publisher::run() {
    st_ = PublisherStats{};
    const auto t0 = clk::now();
    last_send_[0] = last_send_[1] = t0;
    const uint8_t* p = data_;
    uint64_t seq = 1;
    uint64_t first_ts = 0;
    bool have_first_ts = false;

    auto emit = [&](int l, uint64_t s, const Span& sp) {
        if (send_packet(l, s, sp.count, sp)) ++st_.sent[l];
    };

    while (p < end_ && !stop_.load(std::memory_order_relaxed)) {
        const Span sp = take(p, end_, cfg_.payload);
        if (sp.count == 0) break;  // truncated tail

        // Schedule by the first message in the packet.
        if (cfg_.rate > 0) {
            wait_until(t0 + std::chrono::nanoseconds(static_cast<int64_t>(static_cast<double>(seq - 1) * 1e9 / cfg_.rate)),
                       seq);
        } else if (cfg_.speedup > 0 && load_be16(sp.blocks) >= 11) {
            const uint64_t ts = load_be48(sp.blocks + 2 + 5);
            if (!have_first_ts) {
                first_ts = ts;
                have_first_ts = true;
            }
            const double offset_ns = static_cast<double>(ts - first_ts) / cfg_.speedup;
            wait_until(t0 + std::chrono::nanoseconds(static_cast<int64_t>(offset_ns)), seq);
        }

        Action act[2];
        loss_.decide(packet_, act);
        for (int l = 0; l < 2; ++l) {
            if (fd_[l] < 0) continue;
            if (act[l] != Action::Send) log_.push_back({packet_, seq, sp.count, static_cast<uint8_t>(l), act[l]});
            switch (act[l]) {
                case Action::Drop:
                    ++st_.dropped[l];
                    break;
                case Action::Delay:
                    // Held back until after the next packet on this line. A
                    // packet already held goes out first, so at most one waits.
                    if (held_[l].valid) emit(l, held_[l].seq, held_[l].span);
                    held_[l] = {true, seq, sp};
                    ++st_.delayed[l];
                    break;
                case Action::Duplicate:
                    emit(l, seq, sp);
                    emit(l, seq, sp);
                    ++st_.duplicated[l];
                    break;
                case Action::Send:
                    emit(l, seq, sp);
                    if (held_[l].valid) {
                        emit(l, held_[l].seq, held_[l].span);
                        held_[l].valid = false;
                    }
                    break;
            }
        }
        ++packet_;
        ++st_.packets;
        st_.messages += sp.count;
        st_.bytes += kHeaderLen + sp.bytes;
        seq += sp.count;
        next_seq_.store(seq, std::memory_order_release);
        if (cfg_.rate <= 0 && cfg_.speedup <= 0 && (packet_ & 1023) == 0) send_heartbeats(seq, clk::now());
    }
    for (int l = 0; l < 2; ++l)
        if (fd_[l] >= 0 && held_[l].valid) emit(l, held_[l].seq, held_[l].span);
    st_.seconds = std::chrono::duration<double>(clk::now() - t0).count();

    // End of session: the sequence of the next message that would follow.
    for (int i = 0; i < cfg_.end_packets; ++i) {
        for (int l = 0; l < 2; ++l)
            if (fd_[l] >= 0 && !loss_.dead(packet_, l)) send_packet(l, seq, kEndOfSession, Span{});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    finished_.store(true, std::memory_order_release);
    return st_;
}

}  // namespace fh::mold
