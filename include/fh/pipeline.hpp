// The two-thread pipeline: parser thread -> SPSC ring -> book thread.
// Also the single-threaded reference run used for golden comparison.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fh/book/apply.hpp"
#include "fh/book/book_common.hpp"
#include "fh/parser/parser.hpp"
#include "fh/queue/spsc_queue.hpp"
#include "fh/stats/clock.hpp"
#include "fh/stats/histogram.hpp"
#include "fh/util/platform.hpp"
#include "fh/view/seqlock.hpp"
#include "fh/view/snapshot.hpp"

namespace fh {

inline constexpr std::size_t kRingCapacity = std::size_t{1} << 14;  // 16384 x 48 B = 768 KiB
using MsgQueue = SpscQueue<Msg, kRingCapacity>;

struct PipelineOptions {
    int producer_cpu = -1;
    int consumer_cpu = -1;
    // Ring index publication batch (1 = after every message). See SpscQueue.
    std::size_t ring_batch = 1;
    // Per-message clock reads + histograms. Off = pure throughput run (no
    // latency results), to measure what the instrumentation itself costs.
    bool measure_latency = true;
    // Measure latency on a random 1-in-N sample of messages (N a power of
    // two; 1 = every message). Unsampled messages carry no clock reads on
    // either thread, so the pipeline runs close to its uninstrumented speed.
    uint32_t sample_every = 1;
    // Pacing (both off = replay as fast as the parser can go).
    double rate = 0;              // book messages per second, constant spacing
    double speedup = 0;           // follow ITCH timestamps at speedup x real time
    uint64_t pace_from_ns = 0;    // with speedup: run unpaced until this ITCH time
    // Golden comparison: chain the book hash after every message.
    bool verify = false;
    uint64_t checkpoint_every = uint64_t{1} << 20;
    // Live view (off by default; never part of the benchmark numbers).
    Seqlock<BookSnapshot>* snapshot = nullptr;
    std::string view_symbol;
    uint32_t snapshot_every_pow2 = 4096;
};

struct RunResult {
    ParseStats parse;
    LatencyHistogram total, queue, book;  // in clock ticks, sampled messages only
    LatencyHistogram depth;               // ring depth (messages) seen at each sample
    double wall_seconds = 0;
    double consumer_busy_seconds = 0;     // sum of book-update time over samples
    uint64_t book_messages = 0;
    BookCounters counters;
    uint64_t final_hash = 0;
    uint64_t digest = 0;
    std::vector<uint64_t> checkpoints;
    bool producer_pinned = false, consumer_pinned = false;
    bool paced = false;
    std::size_t ring_batch = 1;
    uint32_t sample_every = 1;
    std::size_t queue_capacity = 0;
    std::string queue_name = "spsc";  // set by the caller (the pipeline only knows the type)
    std::string placement;       // free-form label, e.g. "cross-core"
};

// Mean book update over the sampled messages, in ns (0 if nothing was timed).
inline double mean_book_update_ns(const RunResult& r) {
    return r.book.count() ? r.consumer_busy_seconds * 1e9 / static_cast<double>(r.book.count()) : 0.0;
}

// xorshift64: a few cycles, good enough to pick an unbiased random sample.
inline uint64_t xorshift64(uint64_t& x) noexcept {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

// One step of the per-message digest chain.
inline uint64_t chain_digest(uint64_t digest, uint64_t book_hash) noexcept { return mix64(digest ^ book_hash); }

inline uint64_t parse_hhmmss(const std::string& s) {
    unsigned h = 0, m = 0, sec = 0;
    std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec);
    return ((h * 60ull + m) * 60ull + sec) * 1'000'000'000ull;
}

namespace detail {
template <class Book>
void publish_snapshot(const Book& book, uint16_t locate, const Msg& m, uint64_t n, Seqlock<BookSnapshot>& out) {
    BookSnapshot s;
    s.messages = n;
    s.timestamp = m.timestamp;
    s.locate = locate;
    const std::string sym = book.symbol(locate);
    std::memset(s.symbol, ' ', sizeof s.symbol);
    std::memcpy(s.symbol, sym.data(), std::min(sym.size(), sizeof s.symbol));
    book.top_into(locate, Side::Buy, BookSnapshot::kDepth, [&](const LevelView& l) { s.bids[s.n_bids++] = l; });
    book.top_into(locate, Side::Sell, BookSnapshot::kDepth, [&](const LevelView& l) { s.asks[s.n_asks++] = l; });
    out.store(s);
}
}  // namespace detail

// Single-threaded reference: parse and apply inline, no queue, no timing.
template <class Book>
RunResult run_single(const uint8_t* begin, const uint8_t* end, Book& book, bool verify,
                     uint64_t checkpoint_every = uint64_t{1} << 20) {
    RunResult r;
    const auto t0 = std::chrono::steady_clock::now();
    r.parse = parse_buffer(begin, end, [&](const Msg& m) {
        apply(book, m);
        ++r.book_messages;
        if (verify) {
            r.digest = chain_digest(r.digest, book.hash());
            if (r.book_messages % checkpoint_every == 0) r.checkpoints.push_back(r.digest);
        }
    });
    r.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.counters = book.counters();
    r.final_hash = book.hash();
    return r;
}

// Two-thread pipeline over [begin, end). Queue is any type with the
// SpscQueue interface (try_push/push/flush, try_pop/pop, consumer_depth);
// see fh/queue/queue_kinds.hpp for the ones benchmarked.
template <class Queue = MsgQueue, class Book>
RunResult run_pipeline(const uint8_t* begin, const uint8_t* end, Book& book, const PipelineOptions& opt) {
    auto queue = std::make_unique<Queue>();
    queue->set_batch(opt.ring_batch);
    RunResult r;
    r.queue_capacity = Queue::capacity();
    // Messages fully applied by the book thread. Written by the consumer
    // only (its own cache line, so the store is cheap); the producer reads it
    // once, to drain the ring before timestamp-paced replay starts.
    struct alignas(kCacheLine) Counter {
        std::atomic<uint64_t> v{0};
    };
    Counter consumed;
    r.paced = opt.rate > 0 || opt.speedup > 0;
    r.ring_batch = queue->batch();
    if (opt.sample_every == 0 || (opt.sample_every & (opt.sample_every - 1)) != 0)
        throw std::invalid_argument("sample_every must be a power of two");
    r.sample_every = opt.sample_every;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::thread consumer([&] {
        r.consumer_pinned = pin_current_thread(opt.consumer_cpu);
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) cpu_relax();

        char want[8];
        std::memset(want, ' ', 8);
        std::memcpy(want, opt.view_symbol.data(), std::min<std::size_t>(opt.view_symbol.size(), 8));
        int view_locate = -1;
        const uint64_t snap_mask = opt.snapshot_every_pow2 - 1;

        uint64_t n = 0, busy = 0, digest = 0;
        Msg m;
        for (;;) {
            queue->pop(m);
            if (m.type == MsgType::EndOfStream) [[unlikely]]
                break;
            if (m.flags & kMsgSampled) {
                const uint64_t t1 = now_ticks();
                apply(book, m);
                const uint64_t t2 = now_ticks();
                const uint64_t q = t1 > m.stamp ? t1 - m.stamp : 0;
                r.queue.record(q);
                r.book.record(t2 - t1);
                r.total.record(q + (t2 - t1));
                r.depth.record(queue->consumer_depth());
                busy += t2 - t1;
            } else {
                apply(book, m);
            }
            ++n;
            consumed.v.store(n, std::memory_order_relaxed);
            if (opt.verify) {
                digest = chain_digest(digest, book.hash());
                if (n % opt.checkpoint_every == 0) r.checkpoints.push_back(digest);
            }
            if (opt.snapshot) [[unlikely]] {
                if (m.type == MsgType::StockDirectory && std::memcmp(m.symbol, want, 8) == 0) view_locate = m.locate;
                if ((n & snap_mask) == 0 && view_locate >= 0)
                    detail::publish_snapshot(book, static_cast<uint16_t>(view_locate), m, n, *opt.snapshot);
            }
        }
        r.book_messages = n;
        r.digest = digest;
        r.consumer_busy_seconds = static_cast<double>(busy) / ticks_per_second();
    });

    std::thread producer([&] {
        r.producer_pinned = pin_current_thread(opt.producer_cpu);
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) cpu_relax();

        const double tps = ticks_per_second();
        const bool by_rate = opt.rate > 0;
        const bool by_time = !by_rate && opt.speedup > 0;
        const double ticks_per_msg = by_rate ? tps / opt.rate : 0;
        const double ticks_per_itch_ns = by_time ? tps / 1e9 / opt.speedup : 0;
        double next = static_cast<double>(now_ticks());
        uint64_t base_tick = 0, base_ts = 0;
        bool based = false;
        const uint64_t sample_mask = opt.sample_every - 1;
        uint64_t rng = 0x9E3779B97F4A7C15ull;
        uint64_t pushed = 0;

        r.parse = parse_buffer(begin, end, [&](Msg& m) {
            // With timestamp pacing, messages before pace_from only build the
            // book (replayed at full speed, never timed): they would otherwise
            // report backlog, not latency.
            const bool in_window = !by_time || m.timestamp >= opt.pace_from_ns;
            // High bits of xorshift64 are the well-mixed ones.
            const bool sampled = opt.measure_latency && in_window &&
                                 (sample_mask == 0 || ((xorshift64(rng) >> 40) & sample_mask) == 0);
            m.flags = sampled ? kMsgSampled : 0;
            if (by_rate) {
                // Stamp with the scheduled arrival time, not the time we got
                // round to it: if the producer falls behind (ring full), that
                // delay is charged to latency instead of hidden
                // (coordinated-omission correction).
                next += ticks_per_msg;
                const auto target = static_cast<uint64_t>(next);
                while (now_ticks() < target) cpu_relax();
                m.stamp = target;
            } else if (by_time && m.timestamp >= opt.pace_from_ns) {
                if (!based) {
                    // Drain the warm-up backlog so paced replay starts from
                    // an empty ring, then start the clock.
                    queue->flush();
                    while (consumed.v.load(std::memory_order_acquire) < pushed) cpu_relax();
                    based = true;
                    base_tick = now_ticks();
                    base_ts = m.timestamp;
                }
                const auto target =
                    base_tick + static_cast<uint64_t>(static_cast<double>(m.timestamp - base_ts) * ticks_per_itch_ns);
                while (now_ticks() < target) cpu_relax();
                m.stamp = target;
            } else if (sampled) {
                m.stamp = now_ticks();
            }
            queue->push(m);
            ++pushed;
            if (r.paced) queue->flush();  // about to idle until the next arrival
        });
        Msg end_msg{};
        end_msg.type = MsgType::EndOfStream;
        queue->push(end_msg);
        queue->flush();
    });

    while (ready.load() < 2) cpu_relax();
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    r.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.counters = book.counters();
    r.final_hash = book.hash();
    return r;
}

}  // namespace fh
