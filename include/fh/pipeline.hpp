// The two-thread pipeline: parser thread -> SPSC ring -> book thread.
// Also the single-threaded reference run used for golden comparison.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
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
    LatencyHistogram total, queue, book;  // in clock ticks
    double wall_seconds = 0;
    double consumer_busy_seconds = 0;     // sum of book-update time
    uint64_t book_messages = 0;
    BookCounters counters;
    uint64_t final_hash = 0;
    uint64_t digest = 0;
    std::vector<uint64_t> checkpoints;
    bool producer_pinned = false, consumer_pinned = false;
    bool paced = false;
};

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

// Two-thread pipeline over [begin, end).
template <class Book>
RunResult run_pipeline(const uint8_t* begin, const uint8_t* end, Book& book, const PipelineOptions& opt) {
    auto queue = std::make_unique<MsgQueue>();
    RunResult r;
    r.paced = opt.rate > 0 || opt.speedup > 0;
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
            const uint64_t t1 = now_ticks();
            if (m.type == MsgType::EndOfStream) [[unlikely]]
                break;
            apply(book, m);
            const uint64_t t2 = now_ticks();
            ++n;
            const uint64_t q = t1 > m.stamp ? t1 - m.stamp : 0;
            r.queue.record(q);
            r.book.record(t2 - t1);
            r.total.record(q + (t2 - t1));
            busy += t2 - t1;
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

        r.parse = parse_buffer(begin, end, [&](Msg& m) {
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
                    based = true;
                    base_tick = now_ticks();
                    base_ts = m.timestamp;
                }
                const auto target =
                    base_tick + static_cast<uint64_t>(static_cast<double>(m.timestamp - base_ts) * ticks_per_itch_ns);
                while (now_ticks() < target) cpu_relax();
                m.stamp = target;
            } else {
                m.stamp = now_ticks();
            }
            queue->push(m);
        });
        Msg end_msg{};
        end_msg.type = MsgType::EndOfStream;
        queue->push(end_msg);
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
