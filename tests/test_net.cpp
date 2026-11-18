// Publisher -> loopback UDP -> receiver, in one process.
//
//   Net.*        Stage C2 "done when": clean run, both lines live, one line down,
//                byte-for-byte capture
//   Recovery.*   Stage C3 test matrix, each row with three loss seeds
//
// Every scenario must end with the whole stream delivered and every golden
// checkpoint matched (or, after a snapshot, skipped only before it).
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

#include "fh/mold/message_index.hpp"
#include "fh/mold/publisher.hpp"
#include "fh/net/receiver.hpp"
#include "fh/net/udp_rx.hpp"
#include "fh/recovery/servers.hpp"
#include "fh/testing/itch_writer.hpp"
#include "fh/verify/golden_replay.hpp"

using namespace fh;

namespace {

// Publisher rate. Sanitizer builds run 5-15x slower and set FH_NET_RATE lower,
// so the receiver keeps up and only injected loss causes gaps.
double test_rate() {
    static const double r = std::getenv("FH_NET_RATE") ? std::atof(std::getenv("FH_NET_RATE")) : 500'000;
    return r;
}

struct Feed {
    std::vector<uint8_t> bytes;
    golden::File gold;
    std::unique_ptr<mold::MessageIndex> index;
};

const Feed& feed() {
    static const Feed f = [] {
        Feed x;
        x.bytes = synth::synthetic_feed(400'000, 21, 48);
        x.gold = golden::build(x.bytes.data(), x.bytes.data() + x.bytes.size(), 32'768);
        x.index = std::make_unique<mold::MessageIndex>(x.bytes.data(), x.bytes.data() + x.bytes.size());
        return x;
    }();
    return f;
}

struct Scenario {
    mold::PublisherConfig pub;
    net::ReceiverConfig rx;
    std::string capture;
    bool retransmit = false, snapshot = false;
    recovery::RetransmitConfig rt;
    std::chrono::milliseconds server_down{0};  // retransmit server ignores requests this long after start
};

struct Outcome {
    net::ReceiverReport rep;
    mold::PublisherStats pub;
    recovery::RetransmitStats rt;
    recovery::SnapshotStats snap;
};

Outcome run(Scenario s) {
    net::UdpRxConfig u;
    u.line[0] = u.line[1] = "127.0.0.1:0";
    net::UdpRx rx(u);
    s.pub.session = mold::Session::from("NETTEST001");
    if (s.pub.rate == 0 && s.pub.speedup == 0) s.pub.rate = test_rate();
    for (int l = 0; l < 2; ++l) s.pub.line[l] = "127.0.0.1:" + std::to_string(rx.port(l));
    const auto& f = feed();
    mold::Publisher p(f.bytes.data(), f.bytes.data() + f.bytes.size(), s.pub);
    auto next_seq = [&p] { return p.next_seq(); };

    std::unique_ptr<recovery::RetransmitServer> rts;
    std::unique_ptr<recovery::SnapshotServer> snaps;
    if (s.retransmit) {
        rts = std::make_unique<recovery::RetransmitServer>(*f.index, s.pub.session, next_seq, s.rt);
        s.rx.recovery.retransmit = "127.0.0.1:" + std::to_string(rts->port());
        if (s.server_down.count()) rts->set_down(true);
    }
    if (s.snapshot) {
        snaps = std::make_unique<recovery::SnapshotServer>(f.bytes.data(), f.bytes.data() + f.bytes.size(), next_seq);
        s.rx.recovery.snapshot = "127.0.0.1:" + std::to_string(snaps->port());
    }
    s.rx.recovery.max_orders = 1u << 20;
    s.rx.golden = &f.gold;
    s.rx.capture_path = s.capture;
    s.rx.idle_timeout = std::chrono::milliseconds(1500);
    s.rx.recovery_timeout = std::chrono::milliseconds(5000);
    s.rx.warmup_messages = 0;

    Outcome o;
    std::atomic<bool> ready{false};
    s.rx.ready = &ready;
    std::thread t([&] { o.rep = net::run_receiver(rx, s.rx); });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    std::thread revive;
    if (rts && s.server_down.count())
        revive = std::thread([&] {
            std::this_thread::sleep_for(s.server_down);
            rts->set_down(false);
        });
    o.pub = p.run();
    t.join();
    if (revive.joinable()) revive.join();
    if (rts) {
        rts->stop();
        o.rt = rts->stats();
    }
    if (snaps) {
        snaps->stop();
        o.snap = snaps->stats();
    }
    return o;
}

void expect_verified(const net::ReceiverReport& r) {
    EXPECT_TRUE(r.complete) << r.stop_reason;
    EXPECT_EQ(r.checkpoints, feed().gold.checkpoints.size());
    EXPECT_EQ(r.checkpoints_matched + r.checkpoints_skipped, r.checkpoints) << "first mismatch " << r.first_mismatch;
    EXPECT_TRUE(r.final_match);
    EXPECT_TRUE(r.invariants) << r.invariant_error;
    EXPECT_TRUE(r.capacity_ok);
}

}  // namespace

// ---- Stage C2 --------------------------------------------------------------------

// Clean run on one host: every checkpoint matches, no gaps. Both lines are
// live, so every packet also arrives twice and each message is applied once.
TEST(Net, CleanRunMatchesGoldenWithBothLinesLive) {
    const auto o = run({});
    expect_verified(o.rep);
    EXPECT_EQ(o.rep.arb.gaps, 0u);
    // Every packet's second copy is a duplicate, except any still in flight
    // when one line's end of session completes the run.
    EXPECT_LE(o.rep.arb.duplicates, o.pub.packets);
    EXPECT_GE(o.rep.arb.duplicates + 4, o.pub.packets);
    EXPECT_EQ(o.rep.messages, o.pub.messages);
    EXPECT_EQ(o.rep.ts_counts[0] + o.rep.ts_counts[1] + o.rep.ts_counts[2],
              o.rep.arb.packets[0] + o.rep.arb.packets[1]);
    EXPECT_GT(o.rep.wire_to_book_ns.count(), 0u);
}

// Byte-for-byte round trip through the network and the arbiter.
TEST(Net, CaptureReproducesTheFile) {
    char path[] = "/tmp/fh_capture_XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    Scenario s;
    s.capture = path;
    const auto o = run(s);
    expect_verified(o.rep);
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(back == feed().bytes);
    std::remove(path);
}

// Kill line A a third of the way in: line B alone finishes the book.
TEST(Net, LineADiesMidRun) {
    Scenario s;
    s.pub.loss.kill_from[0] = 3000;
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_EQ(o.rep.arb.packets[0], 3000u);
}

// Line A drops 2%, both lines duplicate and reorder: still each message once.
TEST(Net, LinesFillEachOthersGaps) {
    Scenario s;
    s.pub.loss.seed = 77;
    s.pub.loss.drop[0] = 0.02;
    s.pub.loss.duplicate = 0.02;
    s.pub.loss.reorder = 0.02;
    const auto o = run(s);
    expect_verified(o.rep);
}

// ---- Stage C3: the recovery test matrix -------------------------------------------

class Recovery : public ::testing::TestWithParam<uint64_t> {
protected:
    Scenario base() const {
        Scenario s;
        s.retransmit = s.snapshot = true;
        s.pub.loss.seed = GetParam();
        return s;
    }
};

// Random 1% loss on line A only: line B fills every gap, nothing is re-requested.
TEST_P(Recovery, LossOnOneLineIsFilledByTheOther) {
    Scenario s = base();
    s.pub.loss.drop[0] = 0.01;
    s.rx.recovery.gap_timeout = std::chrono::milliseconds(20);  // generous: B is never that late on loopback
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_GT(o.pub.dropped[0], 0u);
    EXPECT_EQ(o.rep.gap_requests, 0u);
    EXPECT_EQ(o.rt.requests, 0u);
    EXPECT_EQ(o.rep.snapshots_loaded, 0u);
}

// Independent 1% loss on both lines: the other line fills most, the few
// packets lost on both are re-requested.
TEST_P(Recovery, IndependentLossOnBothLines) {
    Scenario s = base();
    s.pub.loss.drop[0] = s.pub.loss.drop[1] = 0.01;
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_EQ(o.rep.snapshots_loaded, 0u);
}

// The same packets dropped on both lines: only a re-request can fill them.
TEST_P(Recovery, SameSequenceDroppedOnBothLines) {
    Scenario s = base();
    s.pub.loss.drop_both = 0.005;
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_GT(o.rep.gaps_by_retransmit, 0u);
    EXPECT_GT(o.rep.recovery.packets, 0u);
    EXPECT_EQ(o.rep.snapshots_loaded, 0u);
}

// 500 consecutive packets (about 20,000 messages) lost on both lines: more
// than one round of re-requests (8 x 2048 messages in flight).
TEST_P(Recovery, BurstOf500PacketsOnBothLines) {
    Scenario s = base();
    s.pub.loss.drop_ranges.push_back({2000 + GetParam() * 100, 2499 + GetParam() * 100, 3});
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_GE(o.rep.recovery.rerequests, 10u);
    EXPECT_GT(o.rep.gaps_by_retransmit, 0u);
    EXPECT_EQ(o.rep.snapshots_loaded, 0u);
}

// A gap wider than the reorder buffer: packets that do not fit are counted
// as overflow and the whole range is recovered. The gap timer is long (20 ms,
// ~240 packets at this rate) so the 64-slot buffer fills before the
// re-request goes out (scaled for slower test rates); with the default
// 500 us it is filled first.
TEST_P(Recovery, GapBiggerThanTheReorderBuffer) {
    Scenario s = base();
    s.rx.arb.buffer_packets = 64;
    s.rx.recovery.gap_timeout = std::chrono::milliseconds(static_cast<int>(20 * 500'000 / test_rate()));
    s.pub.loss.drop_ranges.push_back({3000, 3299, 3});
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_GT(o.rep.arb.overflow, 0u);
    EXPECT_EQ(o.rep.snapshots_loaded, 0u);
}

// Duplicates and reordering on both lines, plus some loss: each message once.
TEST_P(Recovery, DuplicatesAndReordering) {
    Scenario s = base();
    s.pub.loss.duplicate = 0.05;
    s.pub.loss.reorder = 0.05;
    s.pub.loss.drop_both = 0.002;
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_GT(o.rep.arb.duplicates, o.pub.packets);
}

// The retransmit server ignores everything for its first 120 ms (at 500 K
// msgs/s; scaled for slower test rates): the client retries with backoff
// (2, 4, 8 ... ms) and recovers once it answers.
TEST_P(Recovery, RetransmitServerDownForAWhile) {
    Scenario s = base();
    s.server_down = std::chrono::milliseconds(static_cast<int>(120 * 500'000 / test_rate()));
    s.pub.loss.drop_ranges.push_back({200, 219, 3});  // inside the down period
    s.rx.recovery.max_retries = 12;
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_GT(o.rt.while_down, 0u);
    EXPECT_GT(o.rep.recovery.retries, 0u);
    EXPECT_GT(o.rep.gaps_by_retransmit, 0u);
    EXPECT_EQ(o.rep.snapshots_loaded, 0u);
}

// The retransmit server only keeps the newest 1,000 messages, so a gap is
// already too old when the request arrives: retries run out and the receiver
// resyncs from a snapshot. The book matches from that point on.
TEST_P(Recovery, GapBeyondRetentionResyncsFromASnapshot) {
    Scenario s = base();
    s.rt.retention = 1000;
    s.rx.recovery.request_timeout = std::chrono::microseconds(500);
    s.rx.recovery.max_retries = 2;
    s.pub.loss.drop_ranges.push_back({4000, 4049, 3});
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_GT(o.rt.refused_retention, 0u);
    EXPECT_GE(o.rep.recovery.exhausted, 1u);
    EXPECT_GE(o.rep.snapshots_loaded, 1u);
    EXPECT_GE(o.rep.gaps_by_snapshot, 1u);
}

// Late join: the receiver's first packet is ~3,000 packets in. The gap from
// sequence 1 is wider than the snapshot threshold, so it goes straight to a
// snapshot; checkpoints before it are skipped, every one after it matches.
TEST_P(Recovery, LateJoinStartsFromASnapshot) {
    Scenario s = base();
    s.rx.recovery.snapshot_threshold = 50'000;
    // Nothing at all reaches the late joiner before it joins: at slow test
    // rates an idle-line heartbeat would otherwise reveal part of the gap
    // early, and that part is (correctly) recovered differently.
    s.pub.heartbeat = std::chrono::seconds(60);
    s.pub.loss.drop_ranges.push_back({0, 2999 + GetParam() * 10, 3});
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_EQ(o.rep.snapshots_loaded, 1u);
    EXPECT_GT(o.rep.checkpoints_skipped, 0u);
    EXPECT_GT(o.rep.checkpoints_matched, 0u);
    EXPECT_EQ(o.rep.recovery.rerequests, 0u);
}

// Line A dies for good, line B loses a little: the run completes on B,
// with re-requests for what B lost.
TEST_P(Recovery, LineADiesPermanently) {
    Scenario s = base();
    s.pub.loss.kill_from[0] = 1500;
    s.pub.loss.drop[1] = 0.003;
    const auto o = run(s);
    expect_verified(o.rep);
    EXPECT_EQ(o.rep.arb.packets[0], 1500u);
}

INSTANTIATE_TEST_SUITE_P(Seeds, Recovery, ::testing::Values(1, 2, 3));
