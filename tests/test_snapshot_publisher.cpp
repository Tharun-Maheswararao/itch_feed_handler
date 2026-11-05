// Version 1.1 snapshot publisher (ZeroMQ PUB + Protobuf). Built only with
// -DFH_ENABLE_PUBLISH=ON. Subscribers here use their own ZeroMQ context and
// TCP on 127.0.0.1, exactly like an external subscriber would (inproc:// only
// works inside one context, and the publisher's context is private).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <zmq.hpp>

#include "fh/book/fast_book.hpp"
#include "fh/pipeline.hpp"
#include "fh/pub/snapshot_publisher.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;
using namespace std::chrono_literals;

namespace {

BookSnapshot make_book(const char* sym, uint64_t messages, int bids, int asks, uint64_t shares = 100) {
    BookSnapshot s;
    s.messages = messages;
    s.timestamp = 34'200'000'000'000ull + messages;  // 09:30:00 + a little
    std::memset(s.symbol, ' ', sizeof s.symbol);
    std::memcpy(s.symbol, sym, std::strlen(sym));
    for (int i = 0; i < bids; ++i) s.bids[s.n_bids++] = LevelView{2'931'900u - 100u * i, 3u + i, shares + i};
    for (int i = 0; i < asks; ++i) s.asks[s.n_asks++] = LevelView{2'932'000u + 100u * i, 1u + i, shares * 2 + i};
    return s;
}

// A subscriber like scripts/sub_snapshots.py: its own context, TCP.
struct Subscriber {
    zmq::context_t ctx{1};
    zmq::socket_t sock{ctx, zmq::socket_type::sub};
    Subscriber(const std::string& endpoint, const std::string& topic, int rcvhwm = 1000, int rcvbuf = 0) {
        sock.set(zmq::sockopt::rcvhwm, rcvhwm);
        if (rcvbuf) sock.set(zmq::sockopt::rcvbuf, rcvbuf);  // small kernel buffer: fills fast
        sock.set(zmq::sockopt::rcvtimeo, 2000);
        sock.set(zmq::sockopt::linger, 0);
        sock.set(zmq::sockopt::subscribe, topic);
        sock.connect(endpoint);
    }
    // One snapshot, or false after the receive timeout.
    bool recv(std::string& topic, DecodedSnapshot& out) {
        zmq::message_t t, body;
        if (!sock.recv(t)) return false;
        EXPECT_TRUE(t.more());
        if (!sock.recv(body)) return false;
        topic = t.to_string();
        return decode_snapshot(body.data(), body.size(), out);
    }
};

PublisherConfig local(std::chrono::milliseconds interval) {
    PublisherConfig c;
    c.endpoint = "tcp://127.0.0.1:*";  // any free port; endpoint() says which
    c.interval = interval;
    return c;
}

}  // namespace

TEST(SnapshotPublisher, EncodeDecodeRoundTripsEveryField) {
    const BookSnapshot full = make_book("AAPL", 123'456'789, 5, 5, uint64_t{1} << 40);  // shares > 32 bits
    DecodedSnapshot d;
    const std::string bytes = encode_snapshot(full, 42, 1'700'000'000'123'456'789);
    ASSERT_TRUE(decode_snapshot(bytes.data(), bytes.size(), d));
    EXPECT_EQ(d.publish_seq, 42u);
    EXPECT_EQ(d.send_time_ns, 1'700'000'000'123'456'789);
    EXPECT_EQ(d.book.messages, full.messages);
    EXPECT_EQ(d.book.timestamp, full.timestamp);
    EXPECT_EQ(std::memcmp(d.book.symbol, full.symbol, sizeof full.symbol), 0);
    ASSERT_EQ(d.book.n_bids, 5);
    ASSERT_EQ(d.book.n_asks, 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(d.book.bids[i], full.bids[i]);
        EXPECT_EQ(d.book.asks[i], full.asks[i]);
    }
    EXPECT_TRUE(same_levels(d.book, full));
}

TEST(SnapshotPublisher, OneSidedAndEmptyBooksRoundTrip) {
    for (const BookSnapshot& s : {make_book("ZVZZT", 1, 3, 0), make_book("Q", 2, 0, 0)}) {
        DecodedSnapshot d;
        const std::string bytes = encode_snapshot(s, 1, 0);
        ASSERT_TRUE(decode_snapshot(bytes.data(), bytes.size(), d));
        EXPECT_TRUE(same_levels(d.book, s));
    }
    DecodedSnapshot d;
    EXPECT_FALSE(decode_snapshot("\xff\xff\xff", 3, d)) << "garbage must not parse";
}

TEST(SnapshotPublisher, SameLevelsIgnoresMessageCountAndTime) {
    const BookSnapshot a = make_book("AAPL", 1, 5, 5);
    BookSnapshot b = make_book("AAPL", 999, 5, 5);
    EXPECT_TRUE(same_levels(a, b));
    b.asks[4].orders += 1;
    EXPECT_FALSE(same_levels(a, b));
    EXPECT_FALSE(same_levels(a, make_book("MSFT", 1, 5, 5)));
    EXPECT_FALSE(same_levels(a, make_book("AAPL", 1, 4, 5)));
}

TEST(SnapshotPublisher, FullDepthMessageIsCompact) {
    const std::string bytes = encode_snapshot(make_book("AAPL", 250'000'000, 5, 5, 1500), 1'000'000,
                                              1'700'000'000'000'000'000);
    std::printf("full 5x5 AAPL snapshot: %zu bytes\n", bytes.size());
    EXPECT_LT(bytes.size(), 200u);  // vs 296 bytes for the in-memory struct
}

TEST(SnapshotPublisher, BindErrorIsReportedToTheCaller) {
    Seqlock<BookSnapshot> snap;
    PublisherConfig c;
    c.endpoint = "tcp://not-an-interface:1";
    EXPECT_THROW((SnapshotPublisher{snap, c}), zmq::error_t);
}

// The book changes on every store; the publisher reads it on its own thread
// and a TCP subscriber receives consistent snapshots in order.
TEST(SnapshotPublisher, SubscriberReceivesConsistentSnapshotsInOrder) {
    Seqlock<BookSnapshot> snap;
    SnapshotPublisher pub(snap, local(2ms));
    Subscriber sub(pub.endpoint(), "AAPL");
    std::atomic<bool> done{false};
    std::thread writer([&] {
        // Every level's shares equal the message count, so a snapshot mixed
        // from two stores would show up as a mismatch below.
        for (uint64_t n = 1; !done.load(std::memory_order_relaxed); ++n) {
            snap.store(make_book("AAPL", n, 5, 5, n));
            std::this_thread::sleep_for(200us);
        }
    });
    uint64_t last_seq = 0, last_msgs = 0;
    int received = 0;
    std::string topic;
    DecodedSnapshot d;
    while (received < 25 && sub.recv(topic, d)) {
        ++received;
        EXPECT_EQ(topic, "AAPL");
        EXPECT_GT(d.publish_seq, last_seq);
        EXPECT_GE(d.book.messages, last_msgs);
        EXPECT_EQ(d.book.bids[0].shares, d.book.messages);
        EXPECT_EQ(d.book.asks[4].shares, 2 * d.book.messages + 4);
        last_seq = d.publish_seq;
        last_msgs = d.book.messages;
    }
    done = true;
    writer.join();
    EXPECT_EQ(received, 25) << "subscriber timed out";
    // The first snapshots can be lost while the subscriber connects (ZeroMQ's
    // "slow joiner"); after that, nothing is lost at this rate.
    EXPECT_GT(pub.stats().published, 0u);
}

TEST(SnapshotPublisher, UnchangedBookIsNotResentUntilHeartbeat) {
    Seqlock<BookSnapshot> snap;
    PublisherConfig c = local(1ms);
    c.heartbeat = 1h;
    SnapshotPublisher pub(snap, c);
    // Other stocks' messages move the count and time; this book stays put.
    for (uint64_t n = 1; n < 100; ++n) {
        snap.store(make_book("AAPL", n, 5, 5));
        std::this_thread::sleep_for(1ms);
    }
    pub.stop();
    const PublisherStats s = pub.stats();
    EXPECT_EQ(s.published, 1u);
    EXPECT_GT(s.unchanged, 20u);

    Seqlock<BookSnapshot> snap2;
    snap2.store(make_book("AAPL", 1, 5, 5));
    PublisherConfig h = local(1ms);
    h.heartbeat = 10ms;
    SnapshotPublisher beat(snap2, h);
    std::this_thread::sleep_for(150ms);
    beat.stop();
    EXPECT_GE(beat.stats().published, 4u) << "an idle book is still resent once per heartbeat";
}

// A subscriber that stops reading must not slow the publisher: ZeroMQ drops
// past the send limit, and stop() still returns promptly. Measured against a
// control on the same machine (no subscriber at all), not a fixed rate, so
// it holds on a slow CI runner too.
TEST(SnapshotPublisher, StalledSubscriberNeverBlocksThePublisher) {
    Seqlock<BookSnapshot> snap;
    snap.store(make_book("AAPL", 1, 5, 5));
    PublisherConfig c = local(0ms);  // read and send as fast as possible
    c.heartbeat = 0ms;               // resend every read: no writer needed
    c.send_hwm = 16;
    auto sends_in_300ms = [&](bool stalled_subscriber, uint64_t* received) {
        SnapshotPublisher pub(snap, c);
        std::unique_ptr<Subscriber> sub;
        if (stalled_subscriber) sub = std::make_unique<Subscriber>(pub.endpoint(), "AAPL", 16, /*rcvbuf=*/4096);
        std::this_thread::sleep_for(300ms);
        const auto t0 = std::chrono::steady_clock::now();
        pub.stop();
        EXPECT_LT(std::chrono::steady_clock::now() - t0, 1500ms) << "stop() must not wait for the subscriber";
        if (sub) {
            // Drain: the subscriber holds at most its queue plus socket buffers.
            sub->sock.set(zmq::sockopt::rcvtimeo, 200);
            std::string topic;
            DecodedSnapshot d;
            while (sub->recv(topic, d)) ++*received;
        }
        return pub.stats().published;
    };
    uint64_t received = 0;
    const uint64_t control = sends_in_300ms(false, nullptr);
    const uint64_t stalled = sends_in_300ms(true, &received);
    std::printf("sends in 300 ms: no subscriber %llu, stalled subscriber %llu (it received %llu)\n",
                (unsigned long long)control, (unsigned long long)stalled, (unsigned long long)received);
    EXPECT_GT(stalled, control / 4) << "a stalled subscriber slowed the publisher down";
    EXPECT_LT(received, stalled) << "snapshots past the send limit are dropped, not queued";
}

// The core promise: publishing never changes what the book computes. Same
// feed, verify on, with and without an active publisher and subscriber.
TEST(SnapshotPublisher, PipelineHashChainUnchangedWithPublishing) {
    const auto feed = synth::synthetic_feed(400'000, 11, 16);
    PipelineOptions opt;
    opt.verify = true;
    opt.checkpoint_every = 4096;
    auto plain_book = std::make_unique<FastBook<true>>();
    const RunResult plain = run_pipeline(feed.data(), feed.data() + feed.size(), *plain_book, opt);

    Seqlock<BookSnapshot> snap;
    opt.snapshot = &snap;
    opt.view_symbol = "SYM3";
    opt.snapshot_every_pow2 = 64;  // far more often than the default 4096
    SnapshotPublisher pub(snap, local(1ms));
    Subscriber sub(pub.endpoint(), "SYM3");
    auto book = std::make_unique<FastBook<true>>();
    const RunResult with_pub = run_pipeline(feed.data(), feed.data() + feed.size(), *book, opt);
    pub.stop();

    EXPECT_EQ(with_pub.book_messages, plain.book_messages);
    EXPECT_EQ(with_pub.checkpoints, plain.checkpoints);
    EXPECT_EQ(with_pub.digest, plain.digest);
    EXPECT_EQ(with_pub.final_hash, plain.final_hash);
    EXPECT_GT(pub.stats().published, 0u);

    // The last snapshot the subscriber gets is the seqlock's final state.
    sub.sock.set(zmq::sockopt::rcvtimeo, 300);
    std::string topic;
    DecodedSnapshot d, last;
    int received = 0;
    while (sub.recv(topic, d)) {
        last = d;
        ++received;
    }
    ASSERT_GT(received, 0);
    BookSnapshot final_snap;
    ASSERT_TRUE(snap.load(final_snap));
    EXPECT_TRUE(same_levels(last.book, final_snap));
}
