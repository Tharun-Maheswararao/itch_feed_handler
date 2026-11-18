// Stage C1: MoldUDP64 framing, the message index, seeded loss injection and
// the replay publisher over loopback UDP.
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "fh/mold/loss.hpp"
#include "fh/mold/message_index.hpp"
#include "fh/mold/moldudp64.hpp"
#include "fh/mold/publisher.hpp"
#include "fh/net/udp.hpp"
#include "fh/testing/itch_writer.hpp"

using namespace fh;
using namespace fh::mold;

namespace {

const std::vector<uint8_t>& feed() {
    static const std::vector<uint8_t> f = synth::synthetic_feed(60'000, 11, 24);
    return f;
}

uint64_t count_blocks(const std::vector<uint8_t>& f) {
    uint64_t n = 0;
    for (const uint8_t* p = f.data(); f.data() + f.size() - p >= 2; p += 2 + load_be16(p)) ++n;
    return n;
}

// Pack a buffer into packets in memory, the way the publisher does.
struct Packet {
    std::vector<uint8_t> bytes;
    Header h;
};
std::vector<Packet> pack(const std::vector<uint8_t>& f, std::size_t payload) {
    std::vector<Packet> out;
    const Session s = Session::from("TEST000001");
    const uint8_t* p = f.data();
    const uint8_t* end = p + f.size();
    uint64_t seq = 1;
    while (p < end) {
        const Span sp = take(p, end, payload);
        if (sp.count == 0) break;
        Packet pk;
        pk.bytes.resize(kHeaderLen + sp.bytes);
        write_header(pk.bytes.data(), s, seq, sp.count);
        std::memcpy(pk.bytes.data() + kHeaderLen, sp.blocks, sp.bytes);
        EXPECT_TRUE(read_header(pk.bytes.data(), pk.bytes.size(), pk.h));
        seq += sp.count;
        out.push_back(std::move(pk));
    }
    return out;
}

}  // namespace

TEST(Mold, HeaderRoundTrip) {
    uint8_t b[kHeaderLen];
    write_header(b, Session::from("ABC"), 0x0102030405060708ull, 0xFFFF);
    Header h;
    ASSERT_TRUE(read_header(b, sizeof b, h));
    EXPECT_EQ(std::string(h.session.id, kSessionLen), "ABC       ");
    EXPECT_EQ(h.seq, 0x0102030405060708ull);
    EXPECT_TRUE(h.end_of_session());
    EXPECT_EQ(h.messages(), 0u);
    EXPECT_FALSE(read_header(b, kHeaderLen - 1, h));
}

TEST(Mold, PacketsStayUnderThePayloadLimit) {
    for (std::size_t payload : {64u, 200u, 1400u, 9000u}) {
        const auto pkts = pack(feed(), payload);
        for (const auto& pk : pkts) {
            ASSERT_TRUE(validate(pk.bytes.data(), pk.bytes.size(), pk.h));
            // A packet only exceeds the limit if it holds a single block that alone does.
            if (pk.h.count > 1) {
                EXPECT_LE(pk.bytes.size(), payload);
            }
        }
    }
}

TEST(Mold, MaxCountIsHonoured) {
    const uint8_t* p = feed().data();
    const Span s = take(p, feed().data() + feed().size(), 1 << 20, 7);
    EXPECT_EQ(s.count, 7);
}

// The C1 "done when" round trip, in memory: decode every packet, concatenate
// the messages with their length prefixes, and get the file back byte for byte.
TEST(Mold, RoundTripReproducesTheFileByteForByte) {
    const auto pkts = pack(feed(), kDefaultPayload);
    std::vector<uint8_t> back;
    uint64_t expect = 1;
    for (const auto& pk : pkts) {
        ASSERT_EQ(pk.h.seq, expect);
        ASSERT_TRUE(for_each_message(pk.bytes.data(), pk.bytes.size(), pk.h, [&](uint64_t seq, const uint8_t* m, uint16_t len) {
            ASSERT_EQ(seq, expect++);
            back.push_back(static_cast<uint8_t>(len >> 8));
            back.push_back(static_cast<uint8_t>(len));
            back.insert(back.end(), m, m + len);
        }));
    }
    EXPECT_EQ(expect - 1, count_blocks(feed()));
    EXPECT_TRUE(back == feed());
}

TEST(Mold, CorruptPacketsAreRejected) {
    auto pkts = pack(feed(), kDefaultPayload);
    auto& pk = pkts[3];
    Header h = pk.h;
    EXPECT_FALSE(validate(pk.bytes.data(), pk.bytes.size() - 1, h));  // truncated
    ++h.count;
    EXPECT_FALSE(validate(pk.bytes.data(), pk.bytes.size(), h));  // count too high
    h.count -= 2;
    EXPECT_FALSE(validate(pk.bytes.data(), pk.bytes.size(), h));  // trailing bytes
}

TEST(Mold, MessageIndexFindsEverySequence) {
    const auto& f = feed();
    MessageIndex idx(f.data(), f.data() + f.size());
    std::vector<const uint8_t*> walk;
    for (const uint8_t* p = f.data(); f.data() + f.size() - p >= 2; p += 2 + load_be16(p)) walk.push_back(p);
    ASSERT_EQ(idx.messages(), walk.size());
    for (uint64_t s = 1; s <= idx.messages(); ++s) ASSERT_EQ(idx.find(s), walk[s - 1]) << s;
    EXPECT_EQ(idx.find(0), nullptr);
    EXPECT_EQ(idx.find(idx.messages() + 1), nullptr);
}

// ---- loss injection ----------------------------------------------------------

namespace {
std::vector<std::array<Action, 2>> decisions(const LossConfig& c, uint64_t n) {
    LossInjector inj(c);
    std::vector<std::array<Action, 2>> out(n);
    for (uint64_t i = 0; i < n; ++i) inj.decide(i, out[i].data());
    return out;
}
}  // namespace

TEST(Loss, SameSeedSameDecisions) {
    LossConfig c;
    c.seed = 99;
    c.drop[0] = 0.05;
    c.drop[1] = 0.02;
    c.drop_both = 0.01;
    c.burst_rate = 0.001;
    c.burst_len = 20;
    c.duplicate = 0.01;
    c.reorder = 0.01;
    EXPECT_EQ(decisions(c, 50'000), decisions(c, 50'000));
    LossConfig d = c;
    d.seed = 100;
    EXPECT_NE(decisions(c, 50'000), decisions(d, 50'000));
}

TEST(Loss, ChangingOneLineNeverShiftsTheOther) {
    LossConfig a;
    a.seed = 5;
    a.drop[0] = 0.03;
    a.reorder = 0.01;
    LossConfig b = a;
    b.drop[1] = 0.5;  // only line B changes
    const auto da = decisions(a, 20'000), db = decisions(b, 20'000);
    for (std::size_t i = 0; i < da.size(); ++i) ASSERT_EQ(da[i][0], db[i][0]) << i;
}

TEST(Loss, RatesAreRoughlyAsConfigured) {
    LossConfig c;
    c.seed = 3;
    c.drop[0] = 0.01;
    const auto d = decisions(c, 200'000);
    uint64_t drops = 0, other = 0;
    for (const auto& x : d) {
        drops += x[0] == Action::Drop;
        other += x[1] != Action::Send;
    }
    EXPECT_NEAR(static_cast<double>(drops) / d.size(), 0.01, 0.002);
    EXPECT_EQ(other, 0u);
}

TEST(Loss, BurstsRangesAndDeadLines) {
    LossConfig c;
    c.seed = 8;
    c.drop_ranges.push_back({100, 599, 3});  // 500 packets, both lines
    c.kill_from[0] = 1000;
    const auto d = decisions(c, 1200);
    for (uint64_t i = 0; i < d.size(); ++i) {
        const bool in_range = i >= 100 && i <= 599;
        EXPECT_EQ(d[i][0] == Action::Drop, in_range || i >= 1000) << i;
        EXPECT_EQ(d[i][1] == Action::Drop, in_range) << i;
    }
    LossConfig b;
    b.seed = 8;
    b.burst_rate = 0.002;
    b.burst_len = 25;
    const auto e = decisions(b, 100'000);
    // Every random drop is part of a run of at least burst_len (runs can merge).
    for (std::size_t i = 0; i < e.size();) {
        if (e[i][0] != Action::Drop) { ++i; continue; }
        std::size_t j = i;
        while (j < e.size() && e[j][0] == Action::Drop) ++j;
        if (j < e.size()) {
            EXPECT_GE(j - i, 25u) << "run at " << i;
        }
        i = j;
    }
}

// ---- publisher over loopback ----------------------------------------------------

namespace {

struct LineCapture {
    std::map<uint64_t, std::vector<uint8_t>> packets;  // by first sequence; duplicates collapse
    uint64_t datagrams = 0, heartbeats = 0, ends = 0, duplicates = 0;
    std::vector<uint64_t> order;  // first sequences in arrival order
};

// Read both lines until each has seen an end-of-session packet, or one has
// and the other stayed silent for 300 ms after it (a dead line), or timeout.
void capture(int fd_a, int fd_b, LineCapture cap[2], std::atomic<bool>& ready) {
    using clk = std::chrono::steady_clock;
    pollfd pf[2] = {{fd_a, POLLIN, 0}, {fd_b, POLLIN, 0}};
    uint8_t buf[kMaxDatagram];
    ready = true;
    const auto deadline = clk::now() + std::chrono::seconds(20);
    clk::time_point first_end{};
    for (;;) {
        const auto now = clk::now();
        if (cap[0].ends && cap[1].ends) break;
        if ((cap[0].ends || cap[1].ends) && first_end == clk::time_point{}) first_end = now;
        if (first_end != clk::time_point{} && now - first_end > std::chrono::milliseconds(300)) break;
        if (now > deadline) break;
        if (poll(pf, 2, 50) <= 0) continue;
        for (int l = 0; l < 2; ++l) {
            for (;;) {
                const ssize_t n = recv(pf[l].fd, buf, sizeof buf, 0);
                if (n <= 0) break;
                Header h;
                ASSERT_TRUE(read_header(buf, static_cast<std::size_t>(n), h));
                ++cap[l].datagrams;
                if (h.heartbeat()) { ++cap[l].heartbeats; continue; }
                if (h.end_of_session()) { ++cap[l].ends; continue; }
                ASSERT_TRUE(validate(buf, static_cast<std::size_t>(n), h));
                cap[l].order.push_back(h.seq);
                if (!cap[l].packets.emplace(h.seq, std::vector<uint8_t>(buf, buf + n)).second) ++cap[l].duplicates;
            }
        }
    }
}

struct Rig {
    int fd[2];
    PublisherConfig cfg;
    Rig() {
        net::SocketOptions o;
        o.buffer_bytes = 8 << 20;
        for (int l = 0; l < 2; ++l) {
            fd[l] = net::open_udp_receiver(net::parse_endpoint("127.0.0.1:0"), o);
            cfg.line[l] = "127.0.0.1:" + std::to_string(net::bound_port(fd[l]));
        }
        cfg.session = Session::from("LOOPBACK01");
    }
    ~Rig() {
        close(fd[0]);
        close(fd[1]);
    }
    PublisherStats run(LineCapture cap[2], std::vector<LogEntry>* log = nullptr) {
        std::atomic<bool> ready{false};
        std::thread t([&] { capture(fd[0], fd[1], cap, ready); });
        while (!ready) std::this_thread::yield();
        Publisher pub(feed().data(), feed().data() + feed().size(), cfg);
        const auto st = pub.run();
        t.join();
        if (log) *log = pub.log();
        return st;
    }
};

std::vector<uint8_t> concat(const LineCapture& c) {
    std::vector<uint8_t> out;
    for (const auto& [seq, pk] : c.packets) out.insert(out.end(), pk.begin() + kHeaderLen, pk.end());
    return out;
}

}  // namespace

// Paced so loopback never overflows a receive buffer: then each line alone
// reproduces the file byte for byte, and the measured rate is the requested one.
TEST(Publisher, EachLineCarriesTheWholeFileAtTheRequestedRate) {
    Rig r;
    r.cfg.rate = 400'000;
    LineCapture cap[2];
    const auto st = r.run(cap);
    EXPECT_EQ(st.messages, count_blocks(feed()));
    EXPECT_EQ(st.send_errors, 0u);
    for (int l = 0; l < 2; ++l) {
        EXPECT_TRUE(concat(cap[l]) == feed()) << "line " << l;
        EXPECT_GE(cap[l].ends, 1u);  // the capture stops at the first of three
        EXPECT_EQ(cap[l].duplicates, 0u);
    }
    // Pace accuracy (C1 "done when"): within a few percent of the request.
    EXPECT_NEAR(st.msgs_per_sec(), r.cfg.rate, r.cfg.rate * 0.05);
}

TEST(Publisher, IdleLinesSendHeartbeats) {
    Rig r;
    r.cfg.rate = 2'000;  // ~one packet per 20 ms
    r.cfg.heartbeat = std::chrono::milliseconds(2);
    const auto small = synth::synthetic_feed(400, 3, 4);
    LineCapture cap[2];
    std::atomic<bool> ready{false};
    std::thread t([&] { capture(r.fd[0], r.fd[1], cap, ready); });
    while (!ready) std::this_thread::yield();
    Publisher pub(small.data(), small.data() + small.size(), r.cfg);
    pub.run();
    t.join();
    EXPECT_GT(cap[0].heartbeats, 10u);
    EXPECT_GT(cap[1].heartbeats, 10u);
}

// The C1 determinism check: the same seed drops the same sequence numbers,
// and what each line is missing is exactly what the log says was dropped.
TEST(Publisher, SeededLossIsDeterministicAndLogged) {
    LossConfig loss;
    loss.seed = 1234;
    loss.drop[0] = 0.02;
    loss.drop[1] = 0.01;
    loss.drop_both = 0.005;
    loss.duplicate = 0.01;
    loss.reorder = 0.01;
    std::vector<LogEntry> log1, log2;
    LineCapture c1[2], c2[2];
    {
        Rig r;
        r.cfg.rate = 400'000;
        r.cfg.loss = loss;
        r.run(c1, &log1);
    }
    {
        Rig r;
        r.cfg.rate = 400'000;
        r.cfg.loss = loss;
        r.run(c2, &log2);
    }
    ASSERT_FALSE(log1.empty());
    ASSERT_EQ(log1.size(), log2.size());
    for (std::size_t i = 0; i < log1.size(); ++i) {
        EXPECT_EQ(log1[i].seq, log2[i].seq);
        EXPECT_EQ(log1[i].line, log2[i].line);
        EXPECT_EQ(log1[i].action, log2[i].action);
    }
    for (int l = 0; l < 2; ++l) {
        std::set<uint64_t> dropped;
        uint64_t dups = 0;
        for (const auto& e : log1) {
            if (e.line != l) continue;
            if (e.action == Action::Drop) dropped.insert(e.seq);
            if (e.action == Action::Duplicate) ++dups;
        }
        std::set<uint64_t> missing;
        for (const auto& pk : pack(feed(), kDefaultPayload))
            if (!c1[l].packets.count(pk.h.seq)) missing.insert(pk.h.seq);
        EXPECT_EQ(missing, dropped) << "line " << l;
        EXPECT_EQ(c1[l].duplicates, dups) << "line " << l;
        // Delayed packets arrive after their successor.
        bool saw_reorder = false;
        for (std::size_t i = 1; i < c1[l].order.size(); ++i) saw_reorder |= c1[l].order[i] < c1[l].order[i - 1];
        EXPECT_TRUE(saw_reorder) << "line " << l;
    }
}

TEST(Publisher, DeadLineGoesSilent) {
    Rig r;
    r.cfg.rate = 400'000;
    r.cfg.loss.kill_from[0] = 50;
    LineCapture cap[2];
    r.run(cap);
    EXPECT_EQ(cap[0].packets.size(), 50u);
    EXPECT_EQ(cap[0].ends, 0u);
    EXPECT_TRUE(concat(cap[1]) == feed());
}
