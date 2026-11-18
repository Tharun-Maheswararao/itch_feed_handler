// MoldUDP64 replay publisher (Version 2, Stage C1).
//
// Packs an ITCH file into MoldUDP64 packets and sends every packet on two
// lines, A and B (two sockets, two destinations: unicast ports or multicast
// groups). Packets are sent straight from the file mapping: the header goes
// in one iovec, the message blocks (already in MoldUDP64 framing in the file)
// in a second.
//
// Pacing: full speed, a fixed message rate, or the original ITCH timestamps
// divided by a speedup factor. A line idle for a heartbeat interval sends a
// heartbeat (count 0, sequence = next message), so a receiver can tell a
// quiet line from a dead one. After the last message each live line sends
// end-of-session packets (count 0xFFFF).
//
// Loss injection (loss.hpp) is applied per packet and line, seeded, and every
// action other than a plain send is logged with its sequence range.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "fh/mold/loss.hpp"
#include "fh/mold/moldudp64.hpp"
#include "fh/net/udp.hpp"

namespace fh::mold {

struct PublisherConfig {
    Session session = Session::from("FH00000001");
    std::string line[2] = {"127.0.0.1:31001", "127.0.0.1:31002"};  // "" = line not used
    net::SocketOptions socket;
    std::size_t payload = kDefaultPayload;  // UDP payload limit, header included

    double rate = 0;     // messages per second; 0 = no rate pacing
    double speedup = 0;  // > 0: pace by ITCH timestamps divided by this factor
    std::chrono::milliseconds heartbeat{1000};
    int end_packets = 3;  // end-of-session packets per line, 1 ms apart

    LossConfig loss;
};

struct LogEntry {
    uint64_t packet;  // packet index, 0-based
    uint64_t seq;     // first message
    uint16_t count;
    uint8_t line;
    Action action;
};

struct PublisherStats {
    uint64_t packets = 0, messages = 0, bytes = 0, heartbeats = 0;
    uint64_t sent[2] = {0, 0}, dropped[2] = {0, 0}, duplicated[2] = {0, 0}, delayed[2] = {0, 0};
    uint64_t send_errors = 0;  // ENOBUFS/ECONNREFUSED and the like: the packet did not leave
    double seconds = 0;
    double msgs_per_sec() const { return seconds > 0 ? static_cast<double>(messages) / seconds : 0; }
};

class Publisher {
public:
    Publisher(const uint8_t* data, const uint8_t* end, PublisherConfig cfg);
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    // Sends the whole buffer (or until stop()), then end of session. Blocks.
    PublisherStats run();
    void stop() { stop_.store(true, std::memory_order_relaxed); }

    // Next sequence number to be published: every message below it has been
    // offered to both lines (sent or deliberately dropped). The retransmit
    // and snapshot servers never serve past it.
    uint64_t next_seq() const { return next_seq_.load(std::memory_order_acquire); }
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    const std::vector<LogEntry>& log() const { return log_; }
    const PublisherConfig& config() const { return cfg_; }

private:
    struct Held {
        bool valid = false;
        uint64_t seq = 0;
        Span span;
    };

    bool send_packet(int line, uint64_t seq, uint16_t count, const Span& s);
    void send_heartbeats(uint64_t seq, std::chrono::steady_clock::time_point now);
    void wait_until(std::chrono::steady_clock::time_point t, uint64_t seq);

    const uint8_t* data_;
    const uint8_t* end_;
    PublisherConfig cfg_;
    int fd_[2] = {-1, -1};
    std::vector<LogEntry> log_;
    PublisherStats st_;
    LossInjector loss_;
    Held held_[2];
    uint64_t packet_ = 0;
    std::chrono::steady_clock::time_point last_send_[2];
    std::atomic<uint64_t> next_seq_{1};
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};
};

}  // namespace fh::mold
