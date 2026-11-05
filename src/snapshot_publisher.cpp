#include "fh/pub/snapshot_publisher.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <zmq.hpp>

#include "book_snapshot.pb.h"

namespace fh {

namespace {

std::string trimmed_symbol(const BookSnapshot& s) {
    std::string sym(s.symbol, sizeof s.symbol);
    while (!sym.empty() && (sym.back() == ' ' || sym.back() == '\0')) sym.pop_back();
    return sym;
}

int64_t wall_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void fill_levels(const google::protobuf::RepeatedPtrField<wire::Level>& in, LevelView* out, uint8_t& n) {
    n = static_cast<uint8_t>(std::min<int>(in.size(), BookSnapshot::kDepth));
    for (int i = 0; i < n; ++i) out[i] = LevelView{in[i].price(), in[i].orders(), in[i].shares()};
}

}  // namespace

bool same_levels(const BookSnapshot& a, const BookSnapshot& b) noexcept {
    if (std::memcmp(a.symbol, b.symbol, sizeof a.symbol) != 0 || a.n_bids != b.n_bids || a.n_asks != b.n_asks)
        return false;
    return std::equal(a.bids, a.bids + a.n_bids, b.bids) && std::equal(a.asks, a.asks + a.n_asks, b.asks);
}

std::string encode_snapshot(const BookSnapshot& s, uint64_t publish_seq, int64_t send_time_ns) {
    wire::BookSnapshot m;
    m.set_symbol(trimmed_symbol(s));
    m.set_itch_time_ns(s.timestamp);
    m.set_book_messages(s.messages);
    m.set_publish_seq(publish_seq);
    m.set_send_time_ns(send_time_ns);
    for (int i = 0; i < s.n_bids; ++i) {
        auto* l = m.add_bids();
        l->set_price(s.bids[i].price);
        l->set_shares(s.bids[i].shares);
        l->set_orders(s.bids[i].orders);
    }
    for (int i = 0; i < s.n_asks; ++i) {
        auto* l = m.add_asks();
        l->set_price(s.asks[i].price);
        l->set_shares(s.asks[i].shares);
        l->set_orders(s.asks[i].orders);
    }
    return m.SerializeAsString();
}

bool decode_snapshot(const void* data, std::size_t size, DecodedSnapshot& out) {
    wire::BookSnapshot m;
    if (!m.ParseFromArray(data, static_cast<int>(size))) return false;
    out = DecodedSnapshot{};
    std::memset(out.book.symbol, ' ', sizeof out.book.symbol);
    std::memcpy(out.book.symbol, m.symbol().data(), std::min(m.symbol().size(), sizeof out.book.symbol));
    out.book.timestamp = m.itch_time_ns();
    out.book.messages = m.book_messages();
    fill_levels(m.bids(), out.book.bids, out.book.n_bids);
    fill_levels(m.asks(), out.book.asks, out.book.n_asks);
    out.publish_seq = m.publish_seq();
    out.send_time_ns = m.send_time_ns();
    return true;
}

struct SnapshotPublisher::Impl {
    const Seqlock<BookSnapshot>& source;
    PublisherConfig cfg;
    zmq::context_t ctx{1};
    std::string bound;
    std::atomic<uint64_t> polls{0}, published{0}, unchanged{0}, bytes{0}, last_bytes{0};
    std::mutex mu;  // only for the sleep between polls, so stop() wakes it at once
    std::condition_variable cv;
    bool stopping = false;
    std::thread thread;

    Impl(const Seqlock<BookSnapshot>& s, PublisherConfig c) : source(s), cfg(std::move(c)) {}

    // The socket is created, used and closed on this thread only (ZeroMQ
    // sockets are not thread safe). Bind errors go back to the constructor.
    void run(std::promise<std::string>& ready) {
        zmq::socket_t sock(ctx, zmq::socket_type::pub);
        try {
            sock.set(zmq::sockopt::sndhwm, cfg.send_hwm);
            // Without a linger limit, closing the socket waits forever for a
            // subscriber that stopped reading.
            sock.set(zmq::sockopt::linger, 200);
            sock.bind(cfg.endpoint);
            ready.set_value(sock.get(zmq::sockopt::last_endpoint));
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }

        BookSnapshot cur, last{};
        bool sent_any = false;
        uint64_t seq = 0;
        auto last_send = std::chrono::steady_clock::time_point{};
        auto poll = [&] {
            polls.fetch_add(1, std::memory_order_relaxed);
            if (!source.load(cur)) return;  // nothing written yet
            const auto now = std::chrono::steady_clock::now();
            if (sent_any && same_levels(cur, last) && now - last_send < cfg.heartbeat) {
                unchanged.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const std::string topic = trimmed_symbol(cur);
            const std::string body = encode_snapshot(cur, ++seq, wall_ns());
            // A PUB socket never blocks: past the send limit it drops. Both
            // calls therefore return at once whatever the subscribers do.
            (void)sock.send(zmq::buffer(topic), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
            (void)sock.send(zmq::buffer(body), zmq::send_flags::dontwait);
            published.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(body.size(), std::memory_order_relaxed);
            last_bytes.store(body.size(), std::memory_order_relaxed);
            last = cur;
            last_send = now;
            sent_any = true;
        };

        std::unique_lock lk(mu);
        while (!stopping) {
            lk.unlock();
            poll();
            lk.lock();
            // A zero wait still sleeps about 1 ms on Linux, so interval 0
            // (as fast as possible) skips it.
            if (cfg.interval.count() > 0) cv.wait_for(lk, cfg.interval, [&] { return stopping; });
        }
        lk.unlock();
        poll();  // the final book, so subscribers see where the replay ended
    }
};

SnapshotPublisher::SnapshotPublisher(const Seqlock<BookSnapshot>& source, PublisherConfig cfg)
    : impl_(std::make_unique<Impl>(source, std::move(cfg))) {
    std::promise<std::string> ready;
    auto bound = ready.get_future();
    // The promise moves into the thread, so it outlives its own set_value.
    impl_->thread = std::thread([this, p = std::move(ready)]() mutable { impl_->run(p); });
    try {
        impl_->bound = bound.get();
    } catch (...) {
        impl_->thread.join();
        throw;
    }
}

SnapshotPublisher::~SnapshotPublisher() { stop(); }

void SnapshotPublisher::stop() {
    if (!impl_->thread.joinable()) return;
    {
        std::lock_guard lk(impl_->mu);
        impl_->stopping = true;
    }
    impl_->cv.notify_one();
    impl_->thread.join();
}

const std::string& SnapshotPublisher::endpoint() const { return impl_->bound; }

PublisherStats SnapshotPublisher::stats() const {
    PublisherStats s;
    s.polls = impl_->polls.load(std::memory_order_relaxed);
    s.published = impl_->published.load(std::memory_order_relaxed);
    s.unchanged = impl_->unchanged.load(std::memory_order_relaxed);
    s.bytes = impl_->bytes.load(std::memory_order_relaxed);
    s.last_bytes = impl_->last_bytes.load(std::memory_order_relaxed);
    return s;
}

}  // namespace fh
