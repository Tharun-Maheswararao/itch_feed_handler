// Version 1.1: publishes the live view's seqlock snapshot over ZeroMQ PUB,
// serialized with Protobuf (proto/book_snapshot.proto).
//
//   book thread --store--> Seqlock<BookSnapshot> --load--> live view
//                                                 \--load--> publisher thread --PUB--> subscribers
//
// The publisher is a second reader of the snapshot the live view already
// reads, on its own thread. The book thread only ever writes the seqlock and
// never waits, so the network cannot slow it down: a slow subscriber makes
// ZeroMQ drop snapshots, it cannot push back. ZeroMQ and Protobuf stay inside
// snapshot_publisher.cpp; this header needs neither.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "fh/view/seqlock.hpp"
#include "fh/view/snapshot.hpp"

namespace fh {

struct PublisherConfig {
    std::string endpoint = "tcp://*:5556";
    std::chrono::milliseconds interval{100};    // how often the snapshot is read
    std::chrono::milliseconds heartbeat{1000};  // resend an unchanged book this often
    int send_hwm = 1000;                        // messages queued per subscriber; beyond it, drop
};

struct PublisherStats {
    uint64_t polls = 0;       // snapshot reads
    uint64_t published = 0;   // snapshots sent
    uint64_t unchanged = 0;   // reads skipped: same levels as the last one sent
    uint64_t bytes = 0;       // serialized bytes sent in total
    uint64_t last_bytes = 0;  // size of the last snapshot sent
};

class SnapshotPublisher {
public:
    // Binds and starts the publisher thread. Throws if the endpoint cannot be
    // bound. `source` must outlive the publisher.
    SnapshotPublisher(const Seqlock<BookSnapshot>& source, PublisherConfig cfg);
    ~SnapshotPublisher();  // stop()
    SnapshotPublisher(const SnapshotPublisher&) = delete;
    SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

    // Publishes the latest book if it changed since the last send, then joins.
    void stop();
    // The endpoint as bound: "tcp://127.0.0.1:*" comes back with the port.
    const std::string& endpoint() const;
    PublisherStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Same symbol and same levels. Ignores the message count and the time, which
// change on every snapshot even when this stock's book did not.
bool same_levels(const BookSnapshot& a, const BookSnapshot& b) noexcept;

// The Protobuf encoding of one snapshot, exposed for tests and for measuring
// message size. decode fills `book` (symbol space padded, locate 0).
struct DecodedSnapshot {
    BookSnapshot book;
    uint64_t publish_seq = 0;
    int64_t send_time_ns = 0;
};
std::string encode_snapshot(const BookSnapshot& s, uint64_t publish_seq, int64_t send_time_ns);
bool decode_snapshot(const void* data, std::size_t size, DecodedSnapshot& out);

}  // namespace fh
