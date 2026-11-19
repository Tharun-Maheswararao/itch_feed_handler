// The publisher-side recovery services (Stage C3).
//
// RetransmitServer answers MoldUDP64 re-requests (session, first sequence,
// count) with ordinary MoldUDP64 packets sent back to the requester. It
// indexes sequence -> file offset at startup, so any request is answered
// without scanning; it caps the messages per request and rate-limits each
// client with a token bucket, as exchanges do, so one client cannot flood it;
// it only serves the newest `retention` messages and never anything the
// publisher has not sent yet.
//
// SnapshotServer serves the order-level book as of a sequence number over
// TCP (snapshot.hpp). It keeps one replica book that replays the file
// forward, so each snapshot is exact by construction. With `follow`, the
// replica also keeps up with what the publisher has sent between requests,
// so a snapshot near the live edge is ready at once instead of after
// replaying the day (the cost: a thread busy replaying all the time).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>

#include "fh/mold/message_index.hpp"
#include "fh/mold/moldudp64.hpp"

namespace fh::recovery {

struct RetransmitConfig {
    std::string listen = "127.0.0.1:0";
    uint64_t retention = std::numeric_limits<uint64_t>::max();  // newest messages that can be re-sent
    uint32_t max_count = 2048;      // messages per request; larger requests are trimmed
    double requests_per_sec = 20000;  // per client
    uint32_t burst = 256;           // per client bucket size
    std::size_t payload = mold::kDefaultPayload;
};

struct RetransmitStats {
    uint64_t requests = 0, served_messages = 0, packets = 0, trimmed = 0;
    uint64_t refused_retention = 0, refused_unpublished = 0, rate_limited = 0, while_down = 0, bad = 0;
};

class RetransmitServer {
public:
    RetransmitServer(const mold::MessageIndex& index, const mold::Session& session, std::function<uint64_t()> next_seq,
                     const RetransmitConfig& cfg = {});
    ~RetransmitServer();
    uint16_t port() const;
    void set_down(bool down);  // a down server ignores every request (tests)
    RetransmitStats stats() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SnapshotStats {
    uint64_t served = 0, bytes = 0, orders = 0, failed = 0;
    uint64_t last_seq = 0;
};

class SnapshotServer {
public:
    // `next_seq` bounds a snapshot to what the publisher has sent.
    SnapshotServer(const uint8_t* data, const uint8_t* end, std::function<uint64_t()> next_seq,
                   const std::string& listen = "127.0.0.1:0", bool follow = false);
    ~SnapshotServer();
    uint16_t port() const;
    SnapshotStats stats() const;
    uint64_t replica_seq() const;                   // last message the replica has applied
    void set_serve_delay(std::chrono::milliseconds d);  // tests: a slow server
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fh::recovery
