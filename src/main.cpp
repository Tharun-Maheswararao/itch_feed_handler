// feed_handler: ITCH 5.0 replay, golden-model check and benchmark driver.
//
//   feed_handler count  FILE [--repeat 2]
//   feed_handler golden FILE [--symbols AAPL,MSFT] [--at 10:00:00,15:59:00] [--check-every N]
//   feed_handler verify FILE [--book fast|golden] [--until HH:MM:SS] [--cpu-producer N --cpu-consumer N]
//   feed_handler bench  FILE [--book fast|golden] [--runs 5] [--warmup 1] [--out DIR] [--label NAME] [--until T]
//                            [--rate MSGS_PER_SEC | --speedup X [--pace-from HH:MM:SS]]
//                            [--cpu-producer N] [--cpu-consumer N] [--ring-batch K] [--no-latency]
//                            [--view SYMBOL [--view-interval-ms 1000] [--record FILE] [--no-draw]]
//   feed_handler single FILE [--book fast|golden] [--cpu N] [--timed] [--until T]
//   feed_handler gen    OUT  [--events N] [--seed S] [--stocks K]
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "fh/book/compare.hpp"
#include "fh/book/fast_book.hpp"
#include "fh/book/golden_book.hpp"
#include "fh/parser/mapped_file.hpp"
#include "fh/pipeline.hpp"
#include "fh/stats/report.hpp"
#include "fh/testing/itch_writer.hpp"
#include "fh/view/live_view.hpp"

using namespace fh;

namespace {

struct Args {
    std::string cmd;
    std::vector<std::string> pos;
    std::map<std::string, std::string> kv;

    Args(int argc, char** argv) {
        if (argc > 1) cmd = argv[1];
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                const std::string key = a.substr(2);
                if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                    kv[key] = argv[++i];
                else
                    kv[key] = "1";
            } else {
                pos.push_back(a);
            }
        }
    }
    std::string get(const std::string& k, const std::string& def = {}) const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    double num(const std::string& k, double def) const {
        auto it = kv.find(k);
        return it == kv.end() ? def : std::stod(it->second);
    }
    bool has(const std::string& k) const { return kv.count(k) != 0; }
    const std::string& file() const {
        if (pos.empty()) throw std::runtime_error("missing FILE argument");
        return pos[0];
    }
};

void usage() {
    std::cerr << "usage: feed_handler {count|golden|verify|bench|single|gen} FILE [options]\n"
                 "  see the header of src/main.cpp or README.md for every option\n";
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, sep);)
        if (!item.empty()) out.push_back(item);
    return out;
}

template <class Book>
std::optional<uint16_t> find_locate(const Book& b, const std::string& sym) {
    for (std::size_t i = 0; i < kMaxLocates; ++i)
        if (b.symbol(static_cast<uint16_t>(i)) == sym) return static_cast<uint16_t>(i);
    return std::nullopt;
}

std::string format_hhmmss(uint64_t ns) {
    const uint64_t s = ns / 1'000'000'000;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600),
                  (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    return buf;
}

// End of the stream: the whole file, or the first message at/after --until.
const uint8_t* stream_end(const Args& a, const MappedFile& f) {
    if (!a.has("until")) return f.end();
    const uint64_t cut = parse_hhmmss(a.get("until"));
    const uint8_t* p = f.data();
    while (f.end() - p >= 2) {
        const uint16_t len = load_be16(p);
        if (len >= 11 && f.end() - p >= 2 + len && load_be48(p + 2 + 5) >= cut) break;
        p += 2 + len;
    }
    std::printf("stream cut at %s: %.2f of %.2f GB\n", a.get("until").c_str(), static_cast<double>(p - f.data()) / 1e9,
                static_cast<double>(f.size()) / 1e9);
    return p;
}

std::string px(uint32_t p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
    return buf;
}

template <class Book>
void print_top_of_book(const Book& b, const std::vector<std::string>& syms) {
    std::printf("%-8s %8s %12s %12s %8s %8s %8s\n", "symbol", "bid_sz", "bid", "ask", "ask_sz", "bid_lv", "ask_lv");
    for (const auto& s : syms) {
        auto loc = find_locate(b, s);
        if (!loc) {
            std::printf("%-8s (not in directory)\n", s.c_str());
            continue;
        }
        auto bid = b.top(*loc, Side::Buy, 1);
        auto ask = b.top(*loc, Side::Sell, 1);
        std::printf("%-8s %8s %12s %12s %8s %8zu %8zu\n", s.c_str(),
                    bid.empty() ? "-" : std::to_string(bid[0].shares).c_str(), bid.empty() ? "-" : px(bid[0].price).c_str(),
                    ask.empty() ? "-" : px(ask[0].price).c_str(), ask.empty() ? "-" : std::to_string(ask[0].shares).c_str(),
                    b.depth(*loc, Side::Buy), b.depth(*loc, Side::Sell));
    }
}

int cmd_count(const Args& a) {
    MappedFile f(a.file());
    const int repeat = static_cast<int>(a.num("repeat", 2));
    std::optional<ParseStats> first;
    bool same = true;
    for (int i = 0; i < repeat; ++i) {
        uint64_t sink = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const ParseStats st = parse_buffer(f.data(), f.end(), [&](const Msg& m) { sink += m.ref ^ m.shares; });
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("pass %d: %llu messages in %.3f s = %.1f M msgs/s, %.2f GB/s (checksum %llx)\n", i + 1,
                    static_cast<unsigned long long>(st.messages), secs, msgs_per_sec(st.messages, secs) / 1e6,
                    static_cast<double>(st.bytes) / secs / 1e9, static_cast<unsigned long long>(sink));
        if (!first)
            first = st;
        else
            same = same && first->counts == st.counts && first->messages == st.messages;
    }
    print_message_mix(std::cout, *first);
    if (repeat > 1) std::printf("counts identical across %d passes: %s\n", repeat, same ? "yes" : "NO");
    return same ? 0 : 1;
}

int cmd_golden(const Args& a) {
    MappedFile f(a.file());
    auto book = std::make_unique<GoldenBook<true>>();
    const auto every = static_cast<uint64_t>(a.num("check-every", 0));
    const auto syms = split(a.get("symbols", "AAPL,MSFT,AMZN,TSLA,SPY,QQQ,NVDA,FB"), ',');
    std::vector<uint64_t> at;
    for (const auto& t : split(a.get("at", "10:00:00,12:00:00,15:59:00"), ',')) at.push_back(parse_hhmmss(t));
    std::size_t next_at = 0;
    uint64_t n = 0, checks = 0;
    std::string err;
    bool ok = true;
    const auto t0 = std::chrono::steady_clock::now();
    const ParseStats st = parse_buffer(f.data(), f.end(), [&](const Msg& m) {
        while (next_at < at.size() && m.timestamp >= at[next_at]) {
            std::printf("\ntop of book at %s (after %llu book msgs)\n", format_hhmmss(at[next_at]).c_str(),
                        static_cast<unsigned long long>(n));
            print_top_of_book(*book, syms);
            ++next_at;
        }
        apply(*book, m);
        ++n;
        if (every && n % every == 0) {
            ++checks;
            if (ok && !book->check_invariants(&err)) {
                ok = false;
                std::printf("INVARIANT FAILED after %llu book msgs: %s\n", static_cast<unsigned long long>(n),
                            err.c_str());
            }
        }
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (ok && !book->check_invariants(&err)) {
        ok = false;
        std::printf("INVARIANT FAILED at end: %s\n", err.c_str());
    }
    print_message_mix(std::cout, st);
    const auto& c = book->counters();
    std::printf("golden single-threaded: %.2f s, %.2f M msgs/s\n", secs, msgs_per_sec(st.messages, secs) / 1e6);
    std::printf("adds %llu exec %llu cancel %llu delete %llu replace %llu | live %llu peak %llu | unknown_ref %llu "
                "overfill %llu duplicate %llu\n",
                (unsigned long long)c.adds, (unsigned long long)c.executes, (unsigned long long)c.cancels,
                (unsigned long long)c.deletes, (unsigned long long)c.replaces, (unsigned long long)c.live_orders,
                (unsigned long long)c.peak_live_orders, (unsigned long long)c.unknown_ref,
                (unsigned long long)c.overfill, (unsigned long long)c.duplicate_ref);
    std::printf("invariant checks: %llu periodic + final -> %s\n", (unsigned long long)checks, ok ? "OK" : "FAILED");
    std::printf("\ntop of book at end of stream\n");
    print_top_of_book(*book, syms);
    return ok ? 0 : 1;
}

PipelineOptions pipeline_options(const Args& a) {
    PipelineOptions o;
    o.producer_cpu = static_cast<int>(a.num("cpu-producer", -1));
    o.consumer_cpu = static_cast<int>(a.num("cpu-consumer", -1));
    o.rate = a.num("rate", 0);
    o.ring_batch = static_cast<std::size_t>(a.num("ring-batch", 1));
    o.measure_latency = !a.has("no-latency");
    o.speedup = a.num("speedup", 0);
    if (a.has("pace-from")) o.pace_from_ns = parse_hhmmss(a.get("pace-from"));
    return o;
}

int cmd_verify(const Args& a) {
    MappedFile f(a.file());
    const std::string which = a.get("book", "fast");
    const uint8_t* end = stream_end(a, f);
    std::printf("[1/3] golden model, single thread ...\n");
    auto golden = std::make_unique<GoldenBook<true>>();
    const RunResult rg = run_single(f.data(), end, *golden, true);
    std::printf("      %llu book msgs, %.2f s, digest %016llx\n", (unsigned long long)rg.book_messages, rg.wall_seconds,
                (unsigned long long)rg.digest);

    std::printf("[2/3] two-thread pipeline with %s book ...\n", which.c_str());
    PipelineOptions opt = pipeline_options(a);
    opt.verify = true;
    bool ok = true;
    std::string err;
    auto check = [&](const RunResult& rp, auto& book) {
        std::printf("      %llu book msgs, %.2f s, digest %016llx\n", (unsigned long long)rp.book_messages,
                    rp.wall_seconds, (unsigned long long)rp.digest);
        std::printf("[3/3] comparing ...\n");
        auto expect = [&](bool cond, const std::string& what) {
            std::printf("      %-58s %s\n", what.c_str(), cond ? "OK" : "MISMATCH");
            ok = ok && cond;
        };
        expect(rp.parse.counts == rg.parse.counts, "message counts by type");
        expect(rp.book_messages == rg.book_messages, "book message count");
        std::size_t first_bad = SIZE_MAX;
        for (std::size_t i = 0; i < std::min(rp.checkpoints.size(), rg.checkpoints.size()); ++i)
            if (rp.checkpoints[i] != rg.checkpoints[i]) {
                first_bad = i;
                break;
            }
        expect(rp.checkpoints.size() == rg.checkpoints.size() && first_bad == SIZE_MAX,
               "per-message hash chain checkpoints (" + std::to_string(rg.checkpoints.size()) + ")");
        if (first_bad != SIZE_MAX)
            std::printf("      first divergence within book msgs (%zu, %zu] x 2^20\n", first_bad, first_bad + 1);
        expect(rp.digest == rg.digest, "final per-message hash chain digest");
        const bool eq = books_equal(*golden, book, &err);
        expect(eq, "full book state (" + std::to_string(book.order_count()) + " live orders, every level)");
        if (!eq) std::printf("      %s\n", err.c_str());
        const bool inv_g = golden->check_invariants(&err);
        expect(inv_g, "golden invariants");
        const bool inv_p = book.check_invariants(&err);
        expect(inv_p, "pipeline book invariants");
        if (!inv_p) std::printf("      %s\n", err.c_str());
    };
    if (which == "golden") {
        auto book = std::make_unique<GoldenBook<true>>();
        check(run_pipeline(f.data(), end, *book, opt), *book);
    } else {
        auto book = std::make_unique<FastBook<true>>(std::size_t{1} << static_cast<int>(a.num("order-capacity-log2", 23)));
        check(run_pipeline(f.data(), end, *book, opt), *book);
        std::printf("      order map grows: %llu\n", (unsigned long long)book->order_map_grows());
    }
    std::printf("%s\n", ok ? "PASS: pipeline book matches the golden model" : "FAIL");
    return ok ? 0 : 1;
}

template <class Book>
int bench_with(const Args& a, Book* /*tag*/) {
    const SystemInfo si = collect_system_info();
    print_system(std::cout, si);
    MappedFile f(a.file());
    std::printf("file     %s (%.2f GB), prefaulting ...\n", a.file().c_str(), static_cast<double>(f.size()) / 1e9);
    f.prefault();
    const uint8_t* end = stream_end(a, f);

    PipelineOptions opt = pipeline_options(a);
    const int runs = static_cast<int>(a.num("runs", 5));
    const int warmup = static_cast<int>(a.num("warmup", 1));
    const std::string label = a.get("label", a.get("book", "fast"));
    const std::size_t cap = std::size_t{1} << static_cast<int>(a.num("order-capacity-log2", 23));

    Seqlock<BookSnapshot> snapshot;
    const bool view = a.has("view");
    if (view) {
        opt.snapshot = &snapshot;
        opt.view_symbol = a.get("view");
    }

    auto make_book = [&] {
        if constexpr (std::is_constructible_v<Book, std::size_t>)
            return std::make_unique<Book>(cap);
        else
            return std::make_unique<Book>();
    };

    std::vector<RunRecord> records;
    for (int i = -warmup; i < runs; ++i) {
        auto book = make_book();
        std::unique_ptr<LiveView> lv;
        if (view) {
            lv = std::make_unique<LiveView>(snapshot, std::chrono::milliseconds(static_cast<int>(a.num("view-interval-ms", 1000))),
                                            i == runs - 1 ? a.get("record") : std::string(), !a.has("no-draw"));
            lv->start();
        }
        RunRecord rec{i + 1, label, run_pipeline(f.data(), end, *book, opt)};
        if (lv) lv->stop();
        if (i < 0) {
            std::printf("warmup %d done: %.2f s\n", i + warmup + 1, rec.result.wall_seconds);
            continue;
        }
        print_run(std::cout, rec);
        if (i == 0) {
            if (!rec.result.producer_pinned || !rec.result.consumer_pinned)
                std::printf("  note: threads not pinned (%s)\n",
#if defined(__APPLE__)
                            "macOS has no affinity API; QoS USER_INTERACTIVE requested"
#else
                            "pass --cpu-producer/--cpu-consumer"
#endif
                );
        }
        records.push_back(std::move(rec));
    }
    const std::size_t med = median_run(records);
    std::printf("\nmedian run by %s: run %d\n", records[med].result.paced ? "p99 latency" : "throughput",
                records[med].run);
    print_message_mix(std::cout, records[med].result.parse);
    const std::string out = a.get("out", "results/" + label);
    write_csvs(out, records, si);
    std::printf("wrote %s/histogram.csv and %s/run_stats.csv\n", out.c_str(), out.c_str());
    return 0;
}

int cmd_bench(const Args& a) {
    const std::string which = a.get("book", "fast");
    if (which == "golden") return bench_with(a, static_cast<GoldenBook<false>*>(nullptr));
    if (which == "fast") return bench_with(a, static_cast<FastBook<false>*>(nullptr));
    throw std::runtime_error("--book must be fast or golden");
}

// Single-threaded parse + book on one (optionally pinned) core. With --timed,
// each book update is bracketed by the same two clock reads the pipeline's
// book thread uses, so "book update" is measured identically in both.
template <class Book>
int single_with(const Args& a, std::unique_ptr<Book> book) {
    MappedFile f(a.file());
    f.prefault();
    const uint8_t* end = stream_end(a, f);
    const bool pinned = pin_current_thread(static_cast<int>(a.num("cpu", -1)));
    const bool timed = a.has("timed");
    LatencyHistogram h;
    uint64_t n = 0, busy = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const ParseStats st = parse_buffer(f.data(), end, [&](const Msg& m) {
        if (timed) {
            const uint64_t t1 = now_ticks();
            apply(*book, m);
            const uint64_t t2 = now_ticks();
            h.record(t2 - t1);
            busy += t2 - t1;
        } else {
            apply(*book, m);
        }
        ++n;
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("single [%s%s] %llu book msgs (%llu total) in %.3f s = %.1f ns per book msg, pinned %s\n",
                a.get("book", "fast").c_str(), timed ? ", timed" : "", (unsigned long long)n,
                (unsigned long long)st.messages, secs, secs * 1e9 / static_cast<double>(n), pinned ? "yes" : "no");
    if (timed)
        std::printf("  book update (t2-t1): mean %.1f ns  p50 %.0f  p90 %.0f  p99 %.0f  p99.9 %.0f ns\n",
                    ticks_to_ns(static_cast<double>(busy)) / static_cast<double>(n),
                    ticks_to_ns(static_cast<double>(h.percentile(0.5))), ticks_to_ns(static_cast<double>(h.percentile(0.9))),
                    ticks_to_ns(static_cast<double>(h.percentile(0.99))), ticks_to_ns(static_cast<double>(h.percentile(0.999))));
    return 0;
}

int cmd_single(const Args& a) {
    if (a.get("book", "fast") == "golden") return single_with(a, std::make_unique<GoldenBook<false>>());
    return single_with(a, std::make_unique<FastBook<false>>(std::size_t{1} << static_cast<int>(a.num("order-capacity-log2", 23))));
}

int cmd_gen(const Args& a) {
    if (a.pos.empty()) throw std::runtime_error("missing OUT argument");
    const auto bytes = synth::synthetic_feed(static_cast<uint64_t>(a.num("events", 1'000'000)),
                                               static_cast<uint32_t>(a.num("seed", 42)),
                                               static_cast<int>(a.num("stocks", 16)));
    std::ofstream out(a.pos[0], std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("wrote %zu bytes to %s\n", bytes.size(), a.pos[0].c_str());
    return out ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // stream progress when piped to tee
    try {
        const Args a(argc, argv);
        if (a.cmd == "count") return cmd_count(a);
        if (a.cmd == "golden") return cmd_golden(a);
        if (a.cmd == "verify") return cmd_verify(a);
        if (a.cmd == "bench") return cmd_bench(a);
        if (a.cmd == "gen") return cmd_gen(a);
        if (a.cmd == "single") return cmd_single(a);
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
