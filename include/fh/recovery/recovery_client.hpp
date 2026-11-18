// The receiver's recovery thread (Stage C3). Never on the hot path: the
// receiver thread hands it gap requests through one SPSC ring and takes
// recovered packets (or "snapshot ready") back through a second one, which it
// checks every loop. A slow retransmit therefore never stalls the receiver.
//
// For each gap [from, to):
//   1. re-request the missing range from the retransmit server, in chunks,
//      several in flight; requests already filled (the receiver publishes its
//      expected sequence) are skipped
//   2. no progress within the timeout: retry, doubling the timeout
//   3. the gap is too large, retries are exhausted, or there is no
//      retransmit server: fetch a book snapshot over TCP into a preallocated
//      buffer and tell the receiver its sequence
// Everything is allocated at construction.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fh/mold/moldudp64.hpp"
#include "fh/queue/spsc_queue.hpp"
#include "fh/stats/histogram.hpp"

namespace fh::recovery {

struct RecoveryConfig {
    std::string retransmit;  // retransmit server host:port ("" = none)
    std::string snapshot;    // snapshot server host:port ("" = none)
    std::chrono::microseconds gap_timeout{500};       // how long a gap waits for the other line
    std::chrono::microseconds request_timeout{2000};  // first retry; doubles each time
    int max_retries = 6;
    uint64_t snapshot_threshold = 1'000'000;  // a gap wider than this goes straight to a snapshot
    uint32_t max_request = 2048;              // messages per re-request
    uint32_t inflight = 8;                    // re-requests outstanding at once
    uint32_t max_orders = 4u << 20;           // snapshot buffer capacity
    int snapshot_attempts = 20;               // connection attempts, 50 ms apart

    bool enabled() const { return !retransmit.empty() || !snapshot.empty(); }
};

struct GapRequest {
    uint64_t from, to;  // missing range [from, to)
    mold::Session session;
};

struct Recovered {
    enum Kind : uint8_t { Packet, Snapshot };
    uint64_t seq;  // Snapshot: the last message it includes
    uint32_t len;
    Kind kind;
    uint8_t data[mold::kMaxDatagram];
};

struct RecoveryStats {
    uint64_t gaps = 0;              // requests from the receiver
    uint64_t rerequests = 0;        // re-request datagrams sent
    uint64_t retries = 0;           // rounds sent without progress since the last
    uint64_t exhausted = 0;         // gaps that ran out of retries
    uint64_t packets = 0;           // recovered packets handed to the receiver
    uint64_t snapshots = 0, snapshot_bytes = 0, snapshot_failures = 0;
};

class RecoveryClient {
public:
    explicit RecoveryClient(const RecoveryConfig& cfg);
    ~RecoveryClient();
    void start();
    void stop();

    // Receiver thread.
    bool request(const GapRequest& r) { return requests_.try_push(r); }
    bool poll(Recovered& out) { return recovered_.try_pop(out); }
    void publish_expected(uint64_t e) { expected_.store(e, std::memory_order_release); }

    // Book thread, after a Snapshot item: the snapshot bytes, then release.
    const uint8_t* snapshot_data() const { return snap_.data(); }
    void snapshot_consumed() { snap_busy_.store(false, std::memory_order_release); }

    RecoveryStats stats() const;  // after stop()

private:
    void run();
    void send_round(const GapRequest& g, uint64_t from);
    void drain_responses();
    bool fetch_snapshot(uint64_t min_seq);
    void hand_over(const Recovered& r);

    RecoveryConfig cfg_;
    int fd_ = -1;
    bool have_retransmit_ = false;
    uint8_t server_addr_[16]{};    // sockaddr_in of the retransmit server
    uint8_t snapshot_addr_[16]{};  // sockaddr_in of the snapshot server
    uint64_t sent_to_ = 0;         // end of the range the last round asked for
    SpscQueue<GapRequest, 64> requests_;
    SpscQueue<Recovered, 1024> recovered_;
    std::vector<uint8_t> snap_;
    std::atomic<bool> snap_busy_{false};
    std::atomic<uint64_t> expected_{1};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    RecoveryStats st_;
    Recovered scratch_{};
};

}  // namespace fh::recovery
