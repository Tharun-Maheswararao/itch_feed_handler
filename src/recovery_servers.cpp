#include "fh/recovery/servers.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "fh/book/apply.hpp"
#include "fh/book/fifo_book.hpp"
#include "fh/net/udp.hpp"
#include "fh/parser/parser.hpp"
#include "fh/recovery/snapshot.hpp"
#include "fh/verify/golden_hashes.hpp"

namespace fh::recovery {

using clk = std::chrono::steady_clock;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a client that hung up must not kill the server
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE on the socket instead
#endif

// ---- retransmit server ---------------------------------------------------------

struct RetransmitServer::Impl {
    const mold::MessageIndex& index;
    mold::Session session;
    std::function<uint64_t()> next_seq;
    RetransmitConfig cfg;
    int fd = -1;
    std::atomic<bool> stopping{false}, down{false};
    mutable std::mutex mu;
    RetransmitStats st;
    std::thread thread;

    struct Client {
        uint32_t ip = 0;
        uint16_t port = 0;
        double tokens = 0;
        clk::time_point last{};
        bool used = false;
    };
    Client clients[64];

    Impl(const mold::MessageIndex& i, const mold::Session& s, std::function<uint64_t()> n, const RetransmitConfig& c)
        : index(i), session(s), next_seq(std::move(n)), cfg(c) {}

    Client& client(const sockaddr_in& a, clk::time_point now) {
        Client* lru = &clients[0];
        for (auto& c : clients) {
            if (c.used && c.ip == a.sin_addr.s_addr && c.port == a.sin_port) return c;
            if (!c.used || c.last < lru->last) lru = &c;
        }
        *lru = Client{a.sin_addr.s_addr, a.sin_port, static_cast<double>(cfg.burst), now, true};
        return *lru;
    }

    void handle(const uint8_t* buf, std::size_t n, const sockaddr_in& from, RetransmitStats& s) {
        ++s.requests;
        mold::Header h;
        if (n != mold::kHeaderLen || !mold::read_header(buf, n, h) || !(h.session == session) || h.count == 0 ||
            h.count == mold::kEndOfSession) {
            ++s.bad;
            return;
        }
        if (down.load(std::memory_order_relaxed)) {
            ++s.while_down;
            return;
        }
        const auto now = clk::now();
        Client& c = client(from, now);
        c.tokens = std::min<double>(cfg.burst, c.tokens + std::chrono::duration<double>(now - c.last).count() *
                                                              cfg.requests_per_sec);
        c.last = now;
        if (c.tokens < 1) {
            ++s.rate_limited;
            return;
        }
        c.tokens -= 1;
        const uint64_t next = next_seq();
        if (h.seq == 0 || h.seq >= next) {
            ++s.refused_unpublished;
            return;
        }
        if (next - h.seq > cfg.retention) {
            ++s.refused_retention;
            return;
        }
        uint64_t count = h.count;
        if (count > cfg.max_count) {
            count = cfg.max_count;
            ++s.trimmed;
        }
        count = std::min(count, next - h.seq);

        const uint8_t* p = index.find(h.seq);
        uint64_t seq = h.seq;
        uint8_t hdr[mold::kHeaderLen];
        while (count > 0 && p) {
            const mold::Span sp = mold::take(p, index.end(), cfg.payload, count);
            if (sp.count == 0) break;
            mold::write_header(hdr, session, seq, sp.count);
            iovec iov[2] = {{hdr, mold::kHeaderLen}, {const_cast<uint8_t*>(sp.blocks), sp.bytes}};
            msghdr m{};
            m.msg_name = const_cast<sockaddr_in*>(&from);
            m.msg_namelen = sizeof from;
            m.msg_iov = iov;
            m.msg_iovlen = 2;
            for (int attempt = 0; attempt < 100 && sendmsg(fd, &m, 0) < 0; ++attempt) {
                if (errno != ENOBUFS && errno != EAGAIN) break;
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
            ++s.packets;
            s.served_messages += sp.count;
            seq += sp.count;
            count -= sp.count;
        }
    }

    void run() {
        pollfd pf{fd, POLLIN, 0};
        uint8_t buf[mold::kMaxDatagram];
        RetransmitStats local;
        while (!stopping.load(std::memory_order_relaxed)) {
            if (::poll(&pf, 1, 20) <= 0) continue;
            for (;;) {
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                const ssize_t n = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                if (n <= 0) break;
                handle(buf, static_cast<std::size_t>(n), from, local);
            }
            std::lock_guard lk(mu);
            st = local;
        }
    }
};

RetransmitServer::RetransmitServer(const mold::MessageIndex& index, const mold::Session& session,
                                   std::function<uint64_t()> next_seq, const RetransmitConfig& cfg)
    : impl_(std::make_unique<Impl>(index, session, std::move(next_seq), cfg)) {
    impl_->fd = net::open_udp_socket(net::parse_endpoint(cfg.listen), 4 << 20);
    impl_->thread = std::thread([this] { impl_->run(); });
}

RetransmitServer::~RetransmitServer() { stop(); }

void RetransmitServer::stop() {
    if (!impl_->thread.joinable()) return;
    impl_->stopping = true;
    impl_->thread.join();
    close(impl_->fd);
}

uint16_t RetransmitServer::port() const { return net::bound_port(impl_->fd); }
void RetransmitServer::set_down(bool d) { impl_->down = d; }
RetransmitStats RetransmitServer::stats() const {
    std::lock_guard lk(impl_->mu);
    return impl_->st;
}

// ---- snapshot server -------------------------------------------------------------

struct SnapshotServer::Impl {
    const uint8_t* data;
    const uint8_t* end;
    std::function<uint64_t()> next_seq;
    int fd = -1;
    std::atomic<bool> stopping{false};
    mutable std::mutex mu;
    SnapshotStats st;
    std::thread thread;

    // The replica: the book after message `seq_done`.
    std::unique_ptr<FifoBook<true>> book = std::make_unique<FifoBook<true>>();
    const uint8_t* p;
    uint64_t seq_done = 0, book_msgs = 0, digest = 0;
    ParseStats ps;
    Msg m{};
    std::vector<uint8_t> out;

    Impl(const uint8_t* d, const uint8_t* e, std::function<uint64_t()> n) : data(d), end(e), next_seq(std::move(n)), p(d) {}

    void advance_to(uint64_t target) {
        while (seq_done < target && end - p >= 2) {
            const uint16_t len = load_be16(p);
            if (static_cast<std::size_t>(end - p) < 2u + len) break;
            if (len > 0 && decode(p + 2, len, m, ps)) {
                apply(*book, m);
                digest = golden::chain(digest, book->hash());
                ++book_msgs;
            }
            p += 2 + len;
            ++seq_done;
        }
    }

    void serialize() { write_snapshot(*book, SnapshotHeader{seq_done, book_msgs, digest, 0, 0}, out); }

    static bool read_all(int c, uint8_t* b, std::size_t n) {
        while (n > 0) {
            const ssize_t r = recv(c, b, n, 0);
            if (r <= 0) return false;
            b += r;
            n -= static_cast<std::size_t>(r);
        }
        return true;
    }
    static bool write_all(int c, const uint8_t* b, std::size_t n) {
        while (n > 0) {
            const ssize_t r = send(c, b, n, kSendFlags);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                return false;
            }
            b += r;
            n -= static_cast<std::size_t>(r);
        }
        return true;
    }

    void serve(int c) {
        timeval tv{5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        uint8_t req[kSnapRequestLen];
        SnapshotStats s;
        {
            std::lock_guard lk(mu);
            s = st;
        }
        if (!read_all(c, req, sizeof req) || std::memcmp(req, kSnapRequest, 4) != 0) {
            ++s.failed;
        } else {
            // Never behind the replica (it only moves forward), never past
            // what the publisher has sent.
            uint64_t target = std::max(load_be64(req + 4), seq_done);
            const uint64_t published = next_seq() - 1;
            target = std::min(target, std::max(published, seq_done));
            advance_to(target);
            serialize();
            if (write_all(c, out.data(), out.size())) {
                ++s.served;
                s.bytes += out.size();
                s.orders += book->order_count();
                s.last_seq = seq_done;
            } else {
                ++s.failed;
            }
        }
        std::lock_guard lk(mu);
        st = s;
    }

    void run() {
        pollfd pf{fd, POLLIN, 0};
        while (!stopping.load(std::memory_order_relaxed)) {
            if (::poll(&pf, 1, 20) <= 0) continue;
            const int c = accept(fd, nullptr, nullptr);
            if (c < 0) continue;
            serve(c);
            close(c);
        }
    }
};

SnapshotServer::SnapshotServer(const uint8_t* data, const uint8_t* end, std::function<uint64_t()> next_seq,
                               const std::string& listen)
    : impl_(std::make_unique<Impl>(data, end, std::move(next_seq))) {
    const net::Endpoint ep = net::parse_endpoint(listen);
    impl_->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->fd < 0) throw std::runtime_error("snapshot server: socket");
    const int one = 1;
    setsockopt(impl_->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(impl_->fd, reinterpret_cast<const sockaddr*>(&ep.addr), sizeof ep.addr) != 0 || ::listen(impl_->fd, 8) != 0)
        throw std::runtime_error("snapshot server: cannot listen on " + listen + ": " + std::strerror(errno));
    impl_->thread = std::thread([this] { impl_->run(); });
}

SnapshotServer::~SnapshotServer() { stop(); }

void SnapshotServer::stop() {
    if (!impl_->thread.joinable()) return;
    impl_->stopping = true;
    impl_->thread.join();
    close(impl_->fd);
}

uint16_t SnapshotServer::port() const { return net::bound_port(impl_->fd); }
SnapshotStats SnapshotServer::stats() const {
    std::lock_guard lk(impl_->mu);
    return impl_->st;
}

}  // namespace fh::recovery
