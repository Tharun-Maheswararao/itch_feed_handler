// Stage C2/C3 arbitration rules, without sockets: packets are built in
// memory and fed to the arbiter in chosen orders.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

#include "fh/arb/arbiter.hpp"
#include "fh/mold/moldudp64.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;
using namespace fh::mold;

namespace {

const std::vector<uint8_t>& feed() {
    static const std::vector<uint8_t> f = synth::synthetic_feed(20'000, 5, 12);
    return f;
}

struct Pkt {
    std::vector<uint8_t> b;
    uint64_t seq;
    uint16_t count;
};

const Session kSession = Session::from("ARBTEST001");

std::vector<Pkt> pack(std::size_t payload, const Session& s = kSession) {
    std::vector<Pkt> out;
    const uint8_t* p = feed().data();
    const uint8_t* end = p + feed().size();
    uint64_t seq = 1;
    while (p < end) {
        const Span sp = take(p, end, payload);
        if (!sp.count) break;
        Pkt k{std::vector<uint8_t>(kHeaderLen + sp.bytes), seq, sp.count};
        write_header(k.b.data(), s, seq, sp.count);
        std::memcpy(k.b.data() + kHeaderLen, sp.blocks, sp.bytes);
        seq += sp.count;
        out.push_back(std::move(k));
    }
    return out;
}

Pkt control(uint64_t seq, uint16_t count) {
    Pkt k{std::vector<uint8_t>(kHeaderLen), seq, count};
    write_header(k.b.data(), kSession, seq, count);
    return k;
}

uint64_t total_messages() {
    uint64_t n = 0;
    for (const uint8_t* p = feed().data(); feed().data() + feed().size() - p >= 2; p += 2 + load_be16(p)) ++n;
    return n;
}

struct Sink {
    std::vector<uint8_t> bytes;
    uint64_t next = 1;
    bool in_order = true;
    uint64_t gaps_closed = 0;
    void on_message(uint64_t seq, const uint8_t* m, uint16_t len, uint64_t, uint8_t) {
        if (seq != next) in_order = false;
        next = seq + 1;
        bytes.push_back(static_cast<uint8_t>(len >> 8));
        bytes.push_back(static_cast<uint8_t>(len));
        bytes.insert(bytes.end(), m, m + len);
    }
    void gap_closed(uint64_t, uint64_t) { ++gaps_closed; }
};

using Arb = arb::Arbiter<Sink>;

void feed_pkt(Arb& a, const Pkt& p, uint8_t line, uint64_t t = 0) {
    a.on_packet(p.b.data(), static_cast<uint32_t>(p.b.size()), line, t);
}

void expect_complete(const Sink& s, const Arb& a) {
    EXPECT_TRUE(s.in_order);
    EXPECT_EQ(a.expected(), total_messages() + 1);
    EXPECT_TRUE(s.bytes == feed());
    EXPECT_FALSE(a.in_gap());
}

}  // namespace

TEST(Arbiter, BothLinesLiveEveryMessageOnce) {
    const auto pk = pack(kDefaultPayload);
    Sink s;
    Arb a(s);
    for (const auto& p : pk) {
        feed_pkt(a, p, 0);
        feed_pkt(a, p, 1);
    }
    expect_complete(s, a);
    EXPECT_EQ(a.stats().duplicates, pk.size());
    EXPECT_EQ(a.stats().gaps, 0u);
}

TEST(Arbiter, LineBLaggingFillsLineAGaps) {
    const auto pk = pack(kDefaultPayload);
    Sink s;
    Arb a(s);
    std::mt19937 rng(7);
    const std::size_t lag = 40;
    for (std::size_t i = 0; i < pk.size() + lag; ++i) {
        if (i < pk.size() && rng() % 100 >= 3) feed_pkt(a, pk[i], 0);  // A loses 3%
        if (i >= lag) feed_pkt(a, pk[i - lag], 1);                      // B is complete but late
    }
    expect_complete(s, a);
    EXPECT_GT(a.stats().gaps, 0u);
    EXPECT_EQ(s.gaps_closed, a.stats().gaps);
    EXPECT_GT(a.stats().buffered, 0u);
}

TEST(Arbiter, ReorderedPacketsAreBufferedAndDrained) {
    auto pk = pack(kDefaultPayload);
    std::mt19937 rng(3);
    for (std::size_t i = 0; i + 1 < pk.size(); ++i)
        if (rng() % 10 == 0) std::swap(pk[i], pk[i + 1]);
    Sink s;
    Arb a(s);
    for (const auto& p : pk) feed_pkt(a, p, 0);
    expect_complete(s, a);
}

// The two lines pack messages differently, so B's packets start in the
// middle of A's: the overlap rule must deliver only the new tail.
TEST(Arbiter, LinesThatPackDifferentlyOverlap) {
    const auto pa = pack(kDefaultPayload), pb = pack(300);
    Sink s;
    Arb a(s);
    std::mt19937 rng(11);
    std::size_t ia = 0, ib = 0;
    while (ia < pa.size() || ib < pb.size()) {
        const bool take_a = ib >= pb.size() || (ia < pa.size() && rng() % 2);
        if (take_a) {
            if (rng() % 100 >= 10) feed_pkt(a, pa[ia], 0);  // 10% lost on A
            ++ia;
        } else {
            feed_pkt(a, pb[ib++], 1);
        }
    }
    expect_complete(s, a);
    EXPECT_GT(a.stats().overlaps, 0u);
}

TEST(Arbiter, GapOnBothLinesWaitsForRecovery) {
    const auto pk = pack(kDefaultPayload);
    Sink s;
    Arb a(s);
    const std::size_t hole = 100;
    for (std::size_t i = 0; i < pk.size(); ++i)
        if (i != hole) {
            feed_pkt(a, pk[i], 0);
            feed_pkt(a, pk[i], 1);
        }
    EXPECT_EQ(a.expected(), pk[hole].seq);
    EXPECT_TRUE(a.in_gap());
    EXPECT_EQ(a.gap_end(), pk[hole + 1].seq);
    feed_pkt(a, pk[hole], arb::kLineRecovery);
    expect_complete(s, a);
    EXPECT_EQ(a.stats().packets[arb::kLineRecovery], 1u);
}

// A gap wider than the buffer: what does not fit is counted, the missing
// range runs to the highest sequence seen, and recovering it completes the book.
TEST(Arbiter, OverflowFallsBackToTheWholeRange) {
    const auto pk = pack(kDefaultPayload);
    Sink s;
    arb::ArbiterConfig c;
    c.buffer_packets = 8;
    Arb a(s, c);
    for (std::size_t i = 0; i < pk.size(); ++i)
        if (i < 10 || i > 30) feed_pkt(a, pk[i], 0);
    EXPECT_GT(a.stats().overflow, 0u);
    EXPECT_EQ(a.gap_end(), a.highest());
    for (std::size_t i = 10; i < pk.size(); ++i) feed_pkt(a, pk[i], arb::kLineRecovery);
    expect_complete(s, a);
}

TEST(Arbiter, HeartbeatAndEndOfSessionRevealATailGap) {
    const auto pk = pack(kDefaultPayload);
    Sink s;
    Arb a(s);
    for (std::size_t i = 0; i + 1 < pk.size(); ++i) feed_pkt(a, pk[i], 0);  // the last packet is lost
    EXPECT_FALSE(a.in_gap());
    const uint64_t next = pk.back().seq + pk.back().count;
    feed_pkt(a, control(next, kHeartbeat), 0);
    EXPECT_TRUE(a.in_gap());
    EXPECT_EQ(a.gap_end(), next);
    feed_pkt(a, control(next, kEndOfSession), 1);
    EXPECT_FALSE(a.done());
    feed_pkt(a, pk.back(), arb::kLineRecovery);
    EXPECT_TRUE(a.done());
    expect_complete(s, a);
}

TEST(Arbiter, SnapshotResetSkipsAhead) {
    const auto pk = pack(kDefaultPayload);
    Sink s;
    Arb a(s);
    // Late join: the first packets ever seen start far from 1.
    for (std::size_t i = 200; i < 260; ++i) feed_pkt(a, pk[i], 0);
    EXPECT_TRUE(a.in_gap());
    EXPECT_EQ(s.bytes.size(), 0u);
    a.reset_to(pk[230].seq, 0);  // snapshot as of pk[230].seq - 1
    EXPECT_EQ(a.expected(), pk[260].seq);  // buffered 230..259 drained
    EXPECT_FALSE(a.in_gap());
    EXPECT_EQ(a.buffered_now(), 0u);  // 200..229 swept as stale
}

TEST(Arbiter, ForeignSessionsAndGarbageAreIgnored) {
    const auto pk = pack(kDefaultPayload), other = pack(kDefaultPayload, Session::from("OTHER00001"));
    Sink s;
    Arb a(s);
    feed_pkt(a, pk[0], 0);
    feed_pkt(a, other[1], 0);
    uint8_t junk[7] = {1, 2, 3};
    a.on_packet(junk, sizeof junk, 0, 0);
    Pkt bad = pk[1];
    bad.b.pop_back();
    feed_pkt(a, bad, 0);
    EXPECT_EQ(a.stats().bad_session, 1u);
    EXPECT_EQ(a.stats().malformed, 2u);
    EXPECT_EQ(a.expected(), pk[1].seq);
}

// Randomised: each packet reaches the arbiter at least once, on either line
// or recovery, with duplicates and local reordering. Every seed must end with
// every message delivered once and in order.
TEST(Arbiter, RandomInterleavingsDeliverEachMessageOnce) {
    const auto pa = pack(kDefaultPayload), pb = pack(700);
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        std::mt19937 rng(seed);
        struct Ev {
            const Pkt* p;
            uint8_t line;
            double t;
        };
        std::vector<Ev> ev;
        for (std::size_t i = 0; i < pa.size(); ++i) {
            const double t = static_cast<double>(i);
            if (rng() % 100 >= 5) ev.push_back({&pa[i], 0, t + (rng() % 100) / 25.0});
            else ev.push_back({&pa[i], arb::kLineRecovery, t + 200});  // recovered late
            if (rng() % 100 < 3) ev.push_back({&pa[i], 0, t + 1.5});  // duplicate
        }
        for (std::size_t i = 0; i < pb.size(); ++i)
            if (rng() % 100 >= 20) ev.push_back({&pb[i], 1, static_cast<double>(i) * pa.size() / pb.size() + 3});
        std::stable_sort(ev.begin(), ev.end(), [](const Ev& x, const Ev& y) { return x.t < y.t; });
        Sink s;
        Arb a(s);
        for (const auto& e : ev) feed_pkt(a, *e.p, e.line);
        ASSERT_TRUE(s.in_order) << "seed " << seed;
        ASSERT_TRUE(s.bytes == feed()) << "seed " << seed << " expected " << a.expected() << " of " << total_messages() + 1 << " bytes " << s.bytes.size() << " vs " << feed().size() << " dups " << a.stats().duplicates << " overlaps " << a.stats().overlaps << " stale " << a.stats().stale << " buffered_now " << a.buffered_now() << " highest " << a.highest();
        ASSERT_EQ(a.buffered_now(), 0u) << "seed " << seed;
    }
}
