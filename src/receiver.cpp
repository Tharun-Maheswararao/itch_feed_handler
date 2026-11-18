#include "fh/net/receiver.hpp"

#include <memory>
#include <thread>

#include "fh/book/apply.hpp"
#include "fh/book/fifo_book.hpp"
#include "fh/net/udp.hpp"
#include "fh/queue/spsc_queue.hpp"
#include "fh/recovery/snapshot.hpp"
#include "fh/util/platform.hpp"

namespace fh::net {

namespace {

using clk = std::chrono::steady_clock;
using Ring = SpscQueue<Msg, 1 << 16>;

// Runs on the receiver thread: the arbiter's in-order output.
struct RxSink {
    RxSink(Ring& r, uint32_t s, std::FILE* c) : ring(r), shift(s), capture(c) {}

    Ring& ring;
    uint32_t shift;
    std::FILE* capture;
    ParseStats parse;
    Msg scratch{};
    uint64_t rng = 0x9E3779B97F4A7C15ull;

    // What happened during the current gap, set by the receive loop.
    bool requested = false, snapshotted = false;
    uint64_t by_line = 0, by_retransmit = 0, by_snapshot = 0;
    LatencyHistogram line_ns, retransmit_ns, snapshot_ns;

    bool sample() noexcept {  // xorshift64: 1 in 2^shift
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (rng >> (64 - shift)) == 0;
    }

    void on_message(uint64_t, const uint8_t* m, uint16_t len, uint64_t rx_ns, uint8_t line) {
        if (capture) {
            uint8_t l[2];
            store_be16(l, len);
            std::fwrite(l, 1, 2, capture);
            std::fwrite(m, 1, len, capture);
        }
        if (len == 0) return;  // a zero-length block: framing only, like the file parser
        ++parse.messages;
        ++parse.counts[m[0]];
        if (!decode(m, len, scratch, parse)) return;
        scratch.stamp = rx_ns;
        // Recovered packets carry the time recovery got them, not a wire
        // arrival: they are applied but never timed.
        scratch.flags = line != arb::kLineRecovery && sample() ? kMsgSampled : 0;
        ring.push(scratch);
    }

    void gap_closed(uint64_t opened, uint64_t closed) {
        const uint64_t d = closed > opened ? closed - opened : 0;
        if (snapshotted) {
            ++by_snapshot;
            snapshot_ns.record(d);
        } else if (requested) {
            ++by_retransmit;
            retransmit_ns.record(d);
        } else {
            ++by_line;
            line_ns.record(d);
        }
        requested = snapshotted = false;
    }
};

struct BookResult {
    uint64_t book_msgs = 0;
    uint64_t checkpoints = 0, matched = 0, skipped = 0, first_mismatch = 0;
    bool final_match = false, invariants = false, capacity_ok = false;
    std::string invariant_error;
    LatencyHistogram latency;
};

void book_loop(Ring& ring, FifoBook<true>& book, const ReceiverConfig& cfg, recovery::RecoveryClient* rec,
               BookResult& r) {
    pin_current_thread(cfg.cpu_book);
    const golden::File* g = cfg.golden;
    uint64_t n = 0, digest = 0;
    std::size_t next = 0;
    auto check = [&] {
        const golden::Checkpoint& want = g->checkpoints[next];
        const golden::Checkpoint mine{n, digest, cfg.verify_queue_order ? book.order_hash() : want.order_hash};
        if (mine == want) ++r.matched;
        else if (!r.first_mismatch) r.first_mismatch = n;
        ++next;
    };
    Msg m;
    for (;;) {
        ring.pop(m);
        if (m.type == MsgType::EndOfStream) break;
        if (m.type == MsgType::SnapshotLoad) {
            // The book after message m.ref, from the recovery thread's buffer.
            // The snapshot also carries the book-message count and level
            // digest at that point, so the per-message chain continues and
            // every later checkpoint is checked in full.
            recovery::SnapshotHeader h;
            recovery::read_snapshot_header(rec->snapshot_data(), h);
            book.clear();
            recovery::load_snapshot(book, rec->snapshot_data());
            rec->snapshot_consumed();
            n = h.book_msgs;
            digest = h.level_digest;
            while (g && next < g->checkpoints.size() && g->checkpoints[next].book_msgs <= n) {
                if (g->checkpoints[next].book_msgs == n) check();
                else {
                    ++r.skipped;
                    ++next;
                }
            }
            continue;
        }
        apply(book, m);
        digest = golden::chain(digest, book.hash());
        ++n;
        if (m.flags & kMsgSampled) {
            const uint64_t now = realtime_ns();
            r.latency.record(now > m.stamp ? now - m.stamp : 0);
        }
        if (g && n % g->every == 0 && next < g->checkpoints.size()) check();
    }
    r.book_msgs = n;
    if (g) {
        r.checkpoints = g->checkpoints.size();
        r.final_match = golden::Checkpoint{n, digest, book.order_hash()} == g->final;
    }
    r.invariants = book.check_invariants(&r.invariant_error);
    r.capacity_ok = book.fifo_counters().capacity_ok();
}

}  // namespace

TsSource ReceiverReport::ts_source() const {
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (ts_counts[i] > ts_counts[best]) best = i;
    return static_cast<TsSource>(best);
}

ReceiverReport run_receiver(RxPath& path, const ReceiverConfig& cfg, const std::atomic<bool>* stop) {
    ReceiverReport rep;
    rep.transport = path.name();
    rep.golden_used = cfg.golden != nullptr;
    rep.queue_order_checked = cfg.verify_queue_order;

    // Everything is allocated here, before the first packet.
    auto ring = std::make_unique<Ring>();
    auto book = std::make_unique<FifoBook<true>>();
    std::FILE* capture = nullptr;
    std::unique_ptr<char[]> capture_buf;
    if (!cfg.capture_path.empty()) {
        capture = std::fopen(cfg.capture_path.c_str(), "wb");
        if (!capture) throw std::runtime_error("cannot write " + cfg.capture_path);
        capture_buf = std::make_unique<char[]>(4 << 20);
        std::setvbuf(capture, capture_buf.get(), _IOFBF, 4 << 20);
    }
    std::unique_ptr<recovery::RecoveryClient> rec;
    if (cfg.recovery.enabled()) {
        rec = std::make_unique<recovery::RecoveryClient>(cfg.recovery);
        rec->start();
        rep.recovery_used = true;
    }
    auto recovered = std::make_unique<recovery::Recovered>();
    RxSink sink(*ring, cfg.sample_shift, capture);
    auto arbiter = std::make_unique<arb::Arbiter<RxSink>>(sink, cfg.arb);
    BookResult br;
    std::thread book_thread([&] { book_loop(*ring, *book, cfg, rec.get(), br); });

    pin_current_thread(cfg.cpu_rx);
    const int64_t sys0 = system_udp_buffer_drops();
    constexpr int kBatch = 64;
    RxPacket pk[kBatch];
    const auto t0 = clk::now();
    auto last_packet = t0;
    auto first_packet = t0;
    bool any = false, warm = false;
    uint64_t alloc_base = 0;
    uint32_t idle_polls = 0;
    const uint64_t gap_timeout_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(cfg.recovery.gap_timeout).count());
    uint64_t requested_to = 0, published_expected = 0;
    auto request_time = t0;
    if (cfg.ready) cfg.ready->store(true, std::memory_order_release);
    if (cfg.announce_ready) {
        std::printf("receiver ready\n");
        std::fflush(stdout);
    }

    for (;;) {
        bool progress = false;
        const int n = path.poll(pk, kBatch);
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                ++rep.ts_counts[static_cast<int>(pk[i].ts_source)];
                arbiter->on_packet(pk[i].data, pk[i].len, pk[i].line, pk[i].rx_ns);
            }
            path.release(n);
            progress = true;
        }

        if (rec) {
            while (rec->poll(*recovered)) {
                progress = true;
                if (recovered->kind == recovery::Recovered::Packet) {
                    arbiter->on_packet(recovered->data, recovered->len, arb::kLineRecovery, realtime_ns());
                } else if (recovered->seq + 1 > arbiter->expected()) {
                    // Load before any later message reaches the book: the
                    // control message goes first, then the buffered packets
                    // after the snapshot drain behind it.
                    Msg c{};
                    c.type = MsgType::SnapshotLoad;
                    c.ref = recovered->seq;
                    ring->push(c);
                    sink.snapshotted = true;
                    arbiter->reset_to(recovered->seq + 1, realtime_ns());
                    ++rep.snapshots_loaded;
                } else {
                    // The gap filled another way while the snapshot was on
                    // its way: loading it would move the book backwards.
                    rec->snapshot_consumed();
                    ++rep.snapshots_discarded;
                }
            }
            // Gap timer: give the other line gap_timeout to fill it first.
            if (arbiter->in_gap() && arbiter->has_session()) {
                const uint64_t e = arbiter->expected();
                const auto now_steady = clk::now();
                const bool outstanding = requested_to > e && now_steady - request_time < cfg.recovery_timeout;
                if (!outstanding && realtime_ns() - arbiter->gap_opened_ns() >= gap_timeout_ns) {
                    const uint64_t to = arbiter->gap_end();
                    if (rec->request({e, to, arbiter->session()})) {
                        requested_to = to;
                        request_time = now_steady;
                        sink.requested = true;
                        ++rep.gap_requests;
                    }
                }
            }
            if (arbiter->expected() != published_expected) {
                published_expected = arbiter->expected();
                rec->publish_expected(published_expected);
            }
        }

        if (progress) {
            if (!any) first_packet = clk::now();
            any = true;
            idle_polls = 0;
            if (!warm && sink.parse.messages >= cfg.warmup_messages) {
                warm = true;
                if (cfg.alloc_count) alloc_base = cfg.alloc_count();
            }
            if (arbiter->done()) {
                rep.complete = true;
                rep.stop_reason = "end of session";
                break;
            }
            continue;
        }
        if (stop && stop->load(std::memory_order_relaxed)) {
            rep.stop_reason = "stopped";
            break;
        }
        // Reading the clock costs more than a poll: only when idle a while.
        if (++idle_polls % 256 == 0) {
            const auto now = clk::now();
            if (!any && now - t0 > cfg.start_timeout) {
                rep.stop_reason = "no packet arrived";
                break;
            }
            const bool recovering = rec && requested_to > arbiter->expected() && now - request_time < cfg.recovery_timeout;
            if (any && now - last_packet > cfg.idle_timeout && !recovering) {
                rep.stop_reason = arbiter->in_gap() ? "idle with an unfilled gap" : "idle before end of session";
                break;
            }
        } else if (idle_polls == 1) {
            last_packet = clk::now();
        }
    }
    if (warm && cfg.alloc_count) {
        rep.alloc_counted = true;
        rep.allocations_after_warmup = cfg.alloc_count() - alloc_base;
    }

    Msg end{};
    end.type = MsgType::EndOfStream;
    ring->push(end);
    book_thread.join();
    if (rec) {
        rec->stop();
        rep.recovery = rec->stats();
    }
    rep.seconds = std::chrono::duration<double>(clk::now() - t0).count();
    rep.active_seconds = any ? std::chrono::duration<double>(clk::now() - first_packet).count() : 0;
    if (capture) std::fclose(capture);

    const int64_t sys1 = system_udp_buffer_drops();
    rep.system_drops = sys0 >= 0 && sys1 >= 0 ? sys1 - sys0 : -1;
    rep.path_drops = path.drops();
    rep.arb = arbiter->stats();
    rep.messages = sink.parse.messages;
    rep.book_msgs = br.book_msgs;
    rep.checkpoints = br.checkpoints;
    rep.checkpoints_matched = br.matched;
    rep.checkpoints_skipped = br.skipped;
    rep.first_mismatch = br.first_mismatch;
    rep.final_match = br.final_match;
    rep.invariants = br.invariants;
    rep.invariant_error = br.invariant_error;
    rep.capacity_ok = br.capacity_ok;
    rep.wire_to_book_ns = br.latency;
    rep.gaps_by_line = sink.by_line;
    rep.gaps_by_retransmit = sink.by_retransmit;
    rep.gaps_by_snapshot = sink.by_snapshot;
    rep.line_fill_ns = sink.line_ns;
    rep.retransmit_fill_ns = sink.retransmit_ns;
    rep.snapshot_fill_ns = sink.snapshot_ns;
    return rep;
}

void print_report(const ReceiverReport& r, std::FILE* out) {
    auto ok = [](bool b) { return b ? "OK" : "FAIL"; };
    auto us = [](const LatencyHistogram& h, double q) { return static_cast<double>(h.percentile(q)) / 1e3; };
    const auto& a = r.arb;
    std::fprintf(out, "receiver: %s, %.2f s, stopped: %s\n", r.transport.c_str(), r.seconds, r.stop_reason);
    std::fprintf(out, "  packets A %llu, B %llu, recovered %llu; heartbeats %llu, end of session %llu\n",
                 (unsigned long long)a.packets[0], (unsigned long long)a.packets[1], (unsigned long long)a.packets[2],
                 (unsigned long long)a.heartbeats, (unsigned long long)a.ends);
    std::fprintf(out, "  messages %llu (book %llu); duplicates %llu, overlaps %llu, buffered %llu, overflow %llu\n",
                 (unsigned long long)r.messages, (unsigned long long)r.book_msgs, (unsigned long long)a.duplicates,
                 (unsigned long long)a.overlaps, (unsigned long long)a.buffered, (unsigned long long)a.overflow);
    std::fprintf(out, "  gaps %llu: filled by the other line %llu, by re-request %llu, by snapshot %llu\n",
                 (unsigned long long)a.gaps, (unsigned long long)r.gaps_by_line,
                 (unsigned long long)r.gaps_by_retransmit, (unsigned long long)r.gaps_by_snapshot);
    if (r.gaps_by_line)
        std::fprintf(out, "    other line: open p50 %.1f us, p99 %.1f us\n", us(r.line_fill_ns, 0.5), us(r.line_fill_ns, 0.99));
    if (r.gaps_by_retransmit)
        std::fprintf(out, "    re-request: recovery time p50 %.1f us, p99 %.1f us, max %.1f us\n",
                     us(r.retransmit_fill_ns, 0.5), us(r.retransmit_fill_ns, 0.99),
                     static_cast<double>(r.retransmit_fill_ns.max()) / 1e3);
    if (r.gaps_by_snapshot)
        std::fprintf(out, "    snapshot: recovery time p50 %.1f ms, max %.1f ms\n", us(r.snapshot_fill_ns, 0.5) / 1e3,
                     static_cast<double>(r.snapshot_fill_ns.max()) / 1e6);
    if (r.recovery_used) {
        const auto& s = r.recovery;
        std::fprintf(out, "  recovery: %llu requests, %llu re-requests sent, %llu retries, %llu exhausted, %llu packets "
                          "recovered; snapshots %llu loaded, %llu discarded, %llu failed (%.1f MB)\n",
                     (unsigned long long)r.gap_requests, (unsigned long long)s.rerequests,
                     (unsigned long long)s.retries, (unsigned long long)s.exhausted, (unsigned long long)s.packets,
                     (unsigned long long)r.snapshots_loaded, (unsigned long long)r.snapshots_discarded,
                     (unsigned long long)s.snapshot_failures, static_cast<double>(s.snapshot_bytes) / 1e6);
    }
    std::fprintf(out, "  kernel drops: path %llu, system-wide %s\n", (unsigned long long)r.path_drops,
                 r.system_drops < 0 ? "unknown" : std::to_string(r.system_drops).c_str());
    if (r.wire_to_book_ns.count()) {
        const auto& h = r.wire_to_book_ns;
        std::fprintf(out, "  wire to book (%s, %llu samples): p50 %.2f us, p90 %.2f, p99 %.2f, p99.9 %.2f, max %.2f\n",
                     ts_source_name(r.ts_source()), (unsigned long long)h.count(), us(h, 0.5), us(h, 0.9),
                     us(h, 0.99), us(h, 0.999), static_cast<double>(h.max()) / 1e3);
    }
    std::fprintf(out, "  %-58s %s\n", "complete (every message to end of session)", ok(r.complete));
    if (r.golden_used) {
        std::fprintf(out, "  %-58s %s (%llu of %llu", r.queue_order_checked
                         ? "checkpoints match golden (level digest + queue order)"
                         : "checkpoints match golden (level digest)",
                     ok(r.checkpoints_matched + r.checkpoints_skipped == r.checkpoints),
                     (unsigned long long)r.checkpoints_matched, (unsigned long long)r.checkpoints);
        if (r.checkpoints_skipped)
            std::fprintf(out, ", %llu before a snapshot", (unsigned long long)r.checkpoints_skipped);
        std::fprintf(out, ")\n");
        if (r.first_mismatch) std::fprintf(out, "    first mismatch at book msg %llu\n", (unsigned long long)r.first_mismatch);
        std::fprintf(out, "  %-58s %s\n", "final book matches golden (level digest + queue order)", ok(r.final_match));
    }
    std::fprintf(out, "  %-58s %s\n", "invariants", ok(r.invariants));
    if (!r.invariants) std::fprintf(out, "    %s\n", r.invariant_error.c_str());
    std::fprintf(out, "  %-58s %s\n", "no pool overflow", ok(r.capacity_ok));
    if (r.alloc_counted)
        std::fprintf(out, "  %-58s %s (%llu)\n", "heap allocations after warmup", ok(r.allocations_after_warmup == 0),
                     (unsigned long long)r.allocations_after_warmup);
    std::fprintf(out, "%s\n", r.passed() ? "PASS" : "FAIL");
}

void write_json(const ReceiverReport& r, std::FILE* out) {
    const auto& h = r.wire_to_book_ns;
    const auto& a = r.arb;
    std::fprintf(out,
                 "{\"transport\": \"%s\", \"timestamp_source\": \"%s\", \"passed\": %s, \"complete\": %s, "
                 "\"stop_reason\": \"%s\", \"messages\": %llu, \"book_msgs\": %llu, \"seconds\": %.3f, "
                 "\"msgs_per_sec\": %.0f, \"samples\": %llu, \"p50_ns\": %llu, \"p90_ns\": %llu, \"p99_ns\": %llu, "
                 "\"p999_ns\": %llu, \"max_ns\": %llu, \"gaps\": %llu, \"gaps_by_line\": %llu, "
                 "\"gaps_by_retransmit\": %llu, \"gaps_by_snapshot\": %llu, \"recovery_p50_ns\": %llu, "
                 "\"recovery_p99_ns\": %llu, \"packets_recovered\": %llu, \"duplicates\": %llu, \"overflow\": %llu, "
                 "\"checkpoints\": %llu, \"checkpoints_matched\": %llu, \"checkpoints_skipped\": %llu, "
                 "\"final_match\": %s, \"queue_order_checked\": %s, \"kernel_drops\": %llu, \"system_drops\": %lld, "
                 "\"allocations_after_warmup\": %lld}\n",
                 r.transport.c_str(), ts_source_name(r.ts_source()), r.passed() ? "true" : "false",
                 r.complete ? "true" : "false", r.stop_reason, (unsigned long long)r.messages,
                 (unsigned long long)r.book_msgs, r.active_seconds,
                 r.active_seconds > 0 ? static_cast<double>(r.messages) / r.active_seconds : 0.0,
                 (unsigned long long)h.count(), (unsigned long long)h.percentile(0.5),
                 (unsigned long long)h.percentile(0.9), (unsigned long long)h.percentile(0.99),
                 (unsigned long long)h.percentile(0.999), (unsigned long long)h.max(), (unsigned long long)a.gaps,
                 (unsigned long long)r.gaps_by_line, (unsigned long long)r.gaps_by_retransmit,
                 (unsigned long long)r.gaps_by_snapshot, (unsigned long long)r.retransmit_fill_ns.percentile(0.5),
                 (unsigned long long)r.retransmit_fill_ns.percentile(0.99), (unsigned long long)r.recovery.packets,
                 (unsigned long long)a.duplicates, (unsigned long long)a.overflow, (unsigned long long)r.checkpoints,
                 (unsigned long long)r.checkpoints_matched, (unsigned long long)r.checkpoints_skipped,
                 r.final_match ? "true" : "false", r.queue_order_checked ? "true" : "false",
                 (unsigned long long)r.path_drops, (long long)r.system_drops,
                 r.alloc_counted ? (long long)r.allocations_after_warmup : -1LL);
}

}  // namespace fh::net
