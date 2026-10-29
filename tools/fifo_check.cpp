// fifo_check FILE GOLDEN_HASHES [--speed]
//
// Stage C0's "done when" on real data:
//   1. FifoBook reproduces every checkpoint of the golden hash file (both the
//      per-message level digest and the queue-order hash), and its invariants
//      hold at the end
//   2. the replay performs zero heap allocations after construction
//   3. no pool overflowed, with peak usage against capacity
//   4. with --speed: single-threaded speed against Version 1's FastBook
// fifo_check FILE --speed-only: just the speed comparison, on any file (for
// example a slice that fits in RAM).
// Linked with fh_alloc_counter.
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "fh/book/apply.hpp"
#include "fh/book/fast_book.hpp"
#include "fh/book/fifo_book.hpp"
#include "fh/parser/mapped_file.hpp"
#include "fh/util/platform.hpp"
#include "fh/parser/parser.hpp"
#include "fh/stats/alloc_counter.hpp"
#include "golden_hashes.hpp"

using namespace fh;
using clk = std::chrono::steady_clock;

template <class Book>
double replay_seconds(const MappedFile& f, Book& book) {
    const auto t0 = clk::now();
    parse_buffer(f.data(), f.end(), [&](const Msg& m) { apply(book, m); });
    return std::chrono::duration<double>(clk::now() - t0).count();
}

// Parse + book, single thread, no instrumentation; best of 2 replays each.
int speed_compare(const MappedFile& f, uint64_t book_msgs) {
    auto best = [&](auto make) {
        double b = 1e300;
        for (int i = 0; i < 2; ++i) {
            auto bk = make();
            b = std::min(b, replay_seconds(f, *bk));
        }
        return b;
    };
    const double t_fifo = best([] { return std::make_unique<FifoBook<false>>(); });
    const double t_fast = best([] { return std::make_unique<FastBook<false>>(); });
    std::printf("speed (parse + book, single thread, best of 2, %llu book msgs):\n", (unsigned long long)book_msgs);
    std::printf("  Version 1 FastBook (aggregated)   %.2f s  %.1f ns/book msg\n", t_fast, t_fast * 1e9 / book_msgs);
    std::printf("  Version 2 FifoBook (order level)  %.2f s  %.1f ns/book msg  (%+.0f%%)\n", t_fifo,
                t_fifo * 1e9 / book_msgs, 100.0 * (t_fifo - t_fast) / t_fast);
    return 0;
}

int main(int argc, char** argv) {
    // Linux: no pinning here (single thread). macOS: ask for a performance core
    // (QoS USER_INTERACTIVE); on battery, default-QoS work lands on E-cores.
    pin_current_thread(-1);
    if (argc < 3) {
        std::fprintf(stderr, "usage: fifo_check ITCH_FILE GOLDEN_HASHES [--speed] | fifo_check ITCH_FILE --speed-only\n");
        return 2;
    }
    const bool speed_only = std::string(argv[2]) == "--speed-only";
    const bool speed = speed_only || (argc > 3 && std::string(argv[3]) == "--speed");
    MappedFile f(argv[1]);
    f.prefault();
    if (speed_only) {
        uint64_t n = 0;
        parse_buffer(f.data(), f.end(), [&](const Msg&) { ++n; });
        return speed_compare(f, n);
    }
    const golden::File gold = golden::read(argv[2]);
    if (gold.every == 0 || gold.checkpoints.empty()) {
        std::fprintf(stderr, "no checkpoints in %s\n", argv[2]);
        return 2;
    }

    // 1-3. Verified replay. Checkpoint comparison stores nothing: it walks
    // the golden list with an index, so the replay itself must not allocate.
    auto book = std::make_unique<FifoBook<true>>();
    uint64_t n = 0, digest = 0, mismatches = 0, first_bad = 0;
    std::size_t next = 0;
    const uint64_t allocs_before = allocation_count();
    const auto t0 = clk::now();
    parse_buffer(f.data(), f.end(), [&](const Msg& m) {
        apply(*book, m);
        digest = golden::chain(digest, book->hash());
        if (++n % gold.every == 0 && next < gold.checkpoints.size()) {
            const golden::Checkpoint mine{n, digest, book->order_hash()};
            if (!(mine == gold.checkpoints[next])) {
                ++mismatches;
                if (!first_bad) first_bad = n;
            }
            ++next;
        }
    });
    const double secs = std::chrono::duration<double>(clk::now() - t0).count();
    const uint64_t allocs = allocation_count() - allocs_before;
    const golden::Checkpoint final_mine{n, digest, book->order_hash()};
    std::string err;
    const bool inv = book->check_invariants(&err);
    const auto& fc = book->fifo_counters();
    const auto u = book->usage();

    auto line = [](const char* what, bool ok) { std::printf("  %-62s %s\n", what, ok ? "OK" : "FAIL"); };
    std::printf("FifoBook, %llu book msgs, %.1f s with checkpoint hashing\n", (unsigned long long)n, secs);
    line("checkpoints match golden (level digest + queue-order hash)", mismatches == 0 && next == gold.checkpoints.size());
    if (mismatches) std::printf("    %llu mismatches, first at book msg %llu\n", (unsigned long long)mismatches,
                                (unsigned long long)first_bad);
    line("final state matches golden", final_mine == gold.final);
    line("invariants (level totals, links, window/treap placement)", inv);
    if (!inv) std::printf("    %s\n", err.c_str());
    std::printf("  %-62s %s (%llu)\n", "heap allocations during replay", allocs == 0 ? "OK" : "FAIL",
                (unsigned long long)allocs);
    line("no pool overflow (orders, levels, order map)", fc.capacity_ok());
    std::printf("  %-62s %s\n", "every stock-side got a price window",
                fc.window_pool_full == 0 ? "yes" : ("no: " + std::to_string(fc.window_pool_full) + " fell back to the treap").c_str());
    std::printf("  pools: orders %u / %u peak, levels %u / %u, windows %u / %u\n", u.orders_peak, u.orders_cap,
                u.levels_peak, u.levels_cap, u.windows_peak, u.windows_cap);
    std::printf("  window moves %llu, levels created outside the window %llu\n", (unsigned long long)fc.recentres,
                (unsigned long long)fc.far_levels);

    const int speed_rc = speed ? speed_compare(f, n) : 0;
    (void)speed_rc;
    const bool pass = mismatches == 0 && next == gold.checkpoints.size() && final_mine == gold.final && inv &&
                      allocs == 0 && fc.capacity_ok();
    std::printf("%s\n", pass ? "PASS: Stage C0 checks" : "FAIL");
    return pass ? 0 : 1;
}
