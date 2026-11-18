// The Version 2 receiving side: an RxPath (kernel sockets, AF_XDP or DPDK)
// feeding the arbiter on one busy-polling receiver thread, which pushes the
// in-order messages through the Version 1 SPSC ring to a book thread running
// the order-level FifoBook.
//
//   receiver thread   poll both lines, arbitrate, decode ITCH, push
//   book thread       pop, apply, chain the level digest, compare checkpoints
//                     with the golden file, record wire-to-book latency
//
// Wire to book: the packet's arrival stamp (from the RxPath, labelled with
// its source) to CLOCK_REALTIME read right after the book update, on 1 in
// 2^sample_shift book messages.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "fh/arb/arbiter.hpp"
#include "fh/net/rx_path.hpp"
#include "fh/parser/parser.hpp"
#include "fh/recovery/recovery_client.hpp"
#include "fh/stats/histogram.hpp"
#include "fh/verify/golden_hashes.hpp"

namespace fh::net {

struct ReceiverConfig {
    int cpu_rx = -1, cpu_book = -1;
    arb::ArbiterConfig arb;
    const golden::File* golden = nullptr;  // verify every checkpoint against it
    // Also walk every queue at each checkpoint (the order-level hash). Exact
    // queue-order proof, but a walk of the whole book: latency runs turn it
    // off and keep the per-message level digest, which costs one hash mix.
    // The final book's queue order is always checked.
    bool verify_queue_order = true;
    recovery::RecoveryConfig recovery;     // gap recovery (Stage C3); off when no server is given
    std::string capture_path;              // write delivered messages in ITCH file framing
    uint32_t sample_shift = 4;             // time 1 in 16 book messages
    std::chrono::milliseconds start_timeout{10'000};  // give up if no packet arrives
    std::chrono::milliseconds idle_timeout{3'000};    // give up after this long with no packet
    std::chrono::milliseconds recovery_timeout{120'000};  // ... unless recovery is still working
    uint64_t (*alloc_count)() = nullptr;   // set to fh::allocation_count when linked with fh_alloc_counter
    uint64_t warmup_messages = 100'000;    // allocations are counted after this many messages
    // Setup (book pools, ring, buffers) takes a few hundred ms. Packets sent
    // before it finishes can overflow the socket buffer, so senders wait for
    // this flag, or for the "receiver ready" line when announce_ready is set.
    std::atomic<bool>* ready = nullptr;
    bool announce_ready = false;
};

struct ReceiverReport {
    std::string transport;
    bool complete = false;  // every message up to the end of session was delivered
    const char* stop_reason = "";
    double seconds = 0;
    double active_seconds = 0;  // first packet to the end: the receive rate's denominator

    arb::ArbStats arb;
    uint64_t messages = 0, book_msgs = 0;
    uint64_t ts_counts[3] = {0, 0, 0};  // packets per TsSource
    TsSource ts_source() const;         // the source most packets carried

    // Verification against the golden file.
    bool golden_used = false;
    uint64_t checkpoints = 0, checkpoints_matched = 0, checkpoints_skipped = 0, first_mismatch = 0;
    bool final_match = false;
    bool invariants = false;
    std::string invariant_error;
    bool capacity_ok = false;

    LatencyHistogram wire_to_book_ns;
    // Gaps by what closed them, and how long each stayed open.
    uint64_t gaps_by_line = 0, gaps_by_retransmit = 0, gaps_by_snapshot = 0;
    LatencyHistogram line_fill_ns, retransmit_fill_ns, snapshot_fill_ns;
    bool queue_order_checked = false;

    bool recovery_used = false;
    recovery::RecoveryStats recovery;
    uint64_t gap_requests = 0, snapshots_loaded = 0, snapshots_discarded = 0;

    uint64_t path_drops = 0;      // the RxPath's own counter (Linux SO_RXQ_OVFL)
    int64_t system_drops = -1;    // system-wide UDP buffer drops during the run, -1 if unknown
    bool alloc_counted = false;
    uint64_t allocations_after_warmup = 0;

    bool passed() const {
        return complete && (!golden_used || (checkpoints_matched + checkpoints_skipped == checkpoints && final_match)) &&
               invariants && capacity_ok && (!alloc_counted || allocations_after_warmup == 0);
    }
};

ReceiverReport run_receiver(RxPath& path, const ReceiverConfig& cfg, const std::atomic<bool>* stop = nullptr);

void print_report(const ReceiverReport& r, std::FILE* out);
// One JSON object with the numbers the benchmark scripts tabulate.
void write_json(const ReceiverReport& r, std::FILE* out);

}  // namespace fh::net
