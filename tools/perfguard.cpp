// perfguard: the measurement core of the performance regression gate.
//
//   perfguard slice IN OUT --messages N
//       Copy the first N ITCH messages of IN (a prefix, so every order a later
//       message refers to was added inside the slice) to OUT. Hash OUT with
//       sha256sum / shasum -a 256 and pin that hash next to the baseline.
//
//   perfguard run FILE [--warmup 1000000] [--runs 7] [--cpu N] [--json OUT]
//       Replay FILE through FifoBook once per run. The first --warmup
//       messages run untimed; every later book message is timed individually
//       (decode + apply) with the serialized timer pair in timer.hpp, minus
//       the measured cost of an empty pair. Each run also replays the same
//       messages with no timers, so the timer's own effect is visible.
//       Exit status 1 if any run allocated after warmup: that check is a
//       hard gate, the timings are data for the gate to compare.
//
// Linked with fh_alloc_counter, so operator new is counted.
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fh/book/apply.hpp"
#include "fh/book/fifo_book.hpp"
#include "fh/parser/mapped_file.hpp"
#include "fh/parser/parser.hpp"
#include "fh/stats/alloc_counter.hpp"
#include "fh/stats/clock.hpp"
#include "fh/stats/histogram.hpp"
#include "fh/stats/timer.hpp"
#include "fh/util/platform.hpp"
#include "fh/util/sysinfo.hpp"

using namespace fh;
using clk = std::chrono::steady_clock;

namespace {

// Byte offset just past the first `n` messages (or the end of the buffer).
struct Prefix {
    const uint8_t* end;
    uint64_t messages;
    uint64_t last_itch_ts;  // ns since midnight of the last message
};

Prefix find_prefix(const uint8_t* p, const uint8_t* end, uint64_t n) {
    Prefix r{p, 0, 0};
    while (r.messages < n && end - p >= 2) {
        const uint16_t len = load_be16(p);
        if (end - p - 2 < len) break;
        if (len >= 11) r.last_itch_ts = load_be48(p + 2 + 5);
        p += 2 + len;
        if (len) ++r.messages;
        r.end = p;
    }
    return r;
}

std::string hms(uint64_t ns) {
    const uint64_t s = ns / 1'000'000'000ull;
    char b[32];
    std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600),
                  (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    return b;
}

int cmd_slice(int argc, char** argv) {
    if (argc < 4) return 2;
    uint64_t n = 10'000'000;
    for (int i = 4; i + 1 < argc; i += 2)
        if (!std::strcmp(argv[i], "--messages")) n = std::stoull(argv[i + 1]);
    MappedFile in(argv[2]);
    const Prefix pre = find_prefix(in.data(), in.end(), n);
    const auto bytes = static_cast<size_t>(pre.end - in.data());
    std::FILE* out = std::fopen(argv[3], "wb");
    if (!out || std::fwrite(in.data(), 1, bytes, out) != bytes || std::fclose(out) != 0) {
        std::perror(argv[3]);
        return 1;
    }
    std::printf("wrote %s: %llu messages, %zu bytes, last message at %s ITCH time\n", argv[3],
                (unsigned long long)pre.messages, bytes, hms(pre.last_itch_ts).c_str());
    if (pre.messages < n) std::printf("note: input ended after %llu messages\n", (unsigned long long)pre.messages);
    return 0;
}

long minor_faults() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

struct RunResult {
    double p50, p90, p99, p999, p9999, max, mean;  // ns, timer overhead removed
    uint64_t book_msgs = 0;
    uint64_t allocs_warmup = 0, allocs_measured = 0;
    long faults_measured = 0;
    double timed_ns_per_msg = 0;    // wall time of the timed loop / book msgs
    double untimed_ns_per_msg = 0;  // same messages, no per-message timers
};

// Replay [begin, warm_end) untimed, then return the measured region's result.
RunResult one_run(const uint8_t* begin, const uint8_t* warm_end, const uint8_t* end, uint64_t overhead,
                  LatencyHistogram& hist) {
    RunResult r{};
    ParseStats st;
    Msg msg{};

    // Timed replay.
    {
        const uint64_t a0 = allocation_count();
        auto book = std::make_unique<FifoBook<false>>();
        parse_buffer(begin, warm_end, [&](const Msg& m) { apply(*book, m); });
        r.allocs_warmup = allocation_count() - a0;

        const uint64_t allocs0 = allocation_count();
        const long f0 = minor_faults();
        const auto w0 = clk::now();
        const uint8_t* p = warm_end;
        while (end - p >= 2) {
            const uint16_t len = load_be16(p);
            p += 2;
            if (len == 0) continue;
            if (end - p < len) [[unlikely]]
                break;
            const uint64_t t0 = timer_start();
            if (decode(p, len, msg, st)) {
                apply(*book, msg);
                const uint64_t d = timer_stop() - t0;
                hist.record(d > overhead ? d - overhead : 0);
            }
            p += len;
        }
        const double secs = std::chrono::duration<double>(clk::now() - w0).count();
        r.faults_measured = minor_faults() - f0;
        r.allocs_measured = allocation_count() - allocs0;
        r.book_msgs = hist.count();
        r.timed_ns_per_msg = secs * 1e9 / static_cast<double>(r.book_msgs ? r.book_msgs : 1);
    }

    // Same work, no per-message timers.
    {
        auto book = std::make_unique<FifoBook<false>>();
        parse_buffer(begin, warm_end, [&](const Msg& m) { apply(*book, m); });
        const auto w0 = clk::now();
        parse_buffer(warm_end, end, [&](const Msg& m) { apply(*book, m); });
        const double secs = std::chrono::duration<double>(clk::now() - w0).count();
        r.untimed_ns_per_msg = secs * 1e9 / static_cast<double>(r.book_msgs ? r.book_msgs : 1);
    }

    auto ns = [](uint64_t t) { return ticks_to_ns(static_cast<double>(t)); };
    r.p50 = ns(hist.percentile(0.50));
    r.p90 = ns(hist.percentile(0.90));
    r.p99 = ns(hist.percentile(0.99));
    r.p999 = ns(hist.percentile(0.999));
    r.p9999 = ns(hist.percentile(0.9999));
    r.max = ns(hist.max());
    r.mean = ticks_to_ns(hist.mean());
    return r;
}

struct Spread {
    double median, min, max;
    double rel() const { return median > 0 ? (max - min) / median : 0; }  // (max - min) / median
};

Spread spread(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    const double med = n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    return {med, v.front(), v.back()};
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o;
}

int cmd_run(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string path = argv[2];
    uint64_t warmup = 1'000'000;
    int runs = 7, cpu = -1;
    std::string json;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--warmup") warmup = std::stoull(argv[i + 1]);
        else if (k == "--runs") runs = std::max(1, std::stoi(argv[i + 1]));
        else if (k == "--cpu") cpu = std::stoi(argv[i + 1]);
        else if (k == "--json") json = argv[i + 1];
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }
    const bool pinned = pin_current_thread(cpu);

    // Replay from a private heap copy, not the mapping. A file written just
    // before the run (CI slices it, then runs) is still settling in the page
    // cache: the first replay of a mapping of it took 2,500+ page faults in
    // the timed region even after prefault(), the next process none. The
    // copy touches every page now, so timings never depend on cache state.
    const std::vector<uint8_t> input = [&] {
        MappedFile f(path);
        return std::vector<uint8_t>(f.data(), f.end());
    }();
    const uint8_t* const base = input.data();
    const uint8_t* const base_end = base + input.size();
    const Prefix warm = find_prefix(base, base_end, warmup);
    const Prefix all = find_prefix(base, base_end, UINT64_MAX);

    const SystemInfo si = collect_system_info();
    const TimerOverhead ovh = calibrate_timer_overhead();
    std::vector<LatencyHistogram> hists(static_cast<std::size_t>(runs));  // allocated before any run
    std::vector<RunResult> res;
    res.reserve(static_cast<std::size_t>(runs));

    std::printf("perfguard: %s, %llu messages (%llu warmup, untimed)\n", path.c_str(),
                (unsigned long long)all.messages, (unsigned long long)warm.messages);
    std::printf("  timer %s, %.3f GHz, resolution %.2f ns, empty pair median %.1f ns (min %.1f, p99 %.1f): subtracted\n",
                timer_name(), ticks_per_second() / 1e9, si.clock_resolution_ns,
                ticks_to_ns(static_cast<double>(ovh.median_ticks)), ticks_to_ns(static_cast<double>(ovh.min_ticks)),
                ticks_to_ns(static_cast<double>(ovh.p99_ticks)));
    std::printf("  cpu %s, pinned: %s\n", si.cpu_model.c_str(), pinned ? std::to_string(cpu).c_str() : "no");
    std::printf("\n  run   p50    p90    p99   p99.9  p99.99      max   (ns)   allocs  faults  ns/msg timed  untimed\n");
    for (int i = 0; i < runs; ++i) {
        const RunResult r =
            one_run(base, warm.end, all.end, ovh.median_ticks, hists[static_cast<std::size_t>(i)]);
        res.push_back(r);
        std::printf("  %3d %5.0f  %5.0f  %5.0f  %6.0f  %6.0f  %9.0f        %6llu  %6ld  %12.1f  %7.1f\n", i + 1, r.p50,
                    r.p90, r.p99, r.p999, r.p9999, r.max, (unsigned long long)r.allocs_measured, r.faults_measured,
                    r.timed_ns_per_msg, r.untimed_ns_per_msg);
    }

    auto col = [&](double RunResult::*m) {
        std::vector<double> v;
        for (const auto& r : res) v.push_back(r.*m);
        return spread(v);
    };
    struct Metric {
        const char* name;
        double RunResult::*m;
    };
    const Metric metrics[] = {{"p50_ns", &RunResult::p50},       {"p90_ns", &RunResult::p90},
                              {"p99_ns", &RunResult::p99},       {"p999_ns", &RunResult::p999},
                              {"p9999_ns", &RunResult::p9999},   {"max_ns", &RunResult::max},
                              {"mean_ns", &RunResult::mean},     {"timed_ns_per_msg", &RunResult::timed_ns_per_msg},
                              {"untimed_ns_per_msg", &RunResult::untimed_ns_per_msg}};
    std::printf("\n  %-20s %10s %10s %10s %8s\n", "median of runs", "median", "min", "max", "spread");
    for (const auto& m : metrics) {
        const Spread s = col(m.m);
        std::printf("  %-20s %10.1f %10.1f %10.1f %7.1f%%\n", m.name, s.median, s.min, s.max, 100 * s.rel());
    }

    uint64_t worst_allocs = 0;
    long worst_faults = 0;  // page faults in a timed region: the gate warns on these
    for (const auto& r : res) {
        worst_allocs = std::max(worst_allocs, r.allocs_measured);
        worst_faults = std::max(worst_faults, r.faults_measured);
    }

    if (!json.empty()) {
        std::FILE* o = std::fopen(json.c_str(), "w");
        if (!o) {
            std::perror(json.c_str());
            return 1;
        }
        std::fprintf(o, "{\n  \"schema\": \"perfguard-run-v1\",\n");
        std::fprintf(o, "  \"input\": {\"path\": \"%s\", \"bytes\": %llu, \"messages\": %llu, \"warmup\": %llu},\n",
                     json_escape(path).c_str(), (unsigned long long)(all.end - base),
                     (unsigned long long)all.messages, (unsigned long long)warm.messages);
        std::fprintf(o,
                     "  \"host\": {\"cpu\": \"%s\", \"os\": \"%s\", \"compiler\": \"%s\", \"flags\": \"%s\", "
                     "\"pinned_cpu\": %d},\n",
                     json_escape(si.cpu_model).c_str(), json_escape(si.os).c_str(), json_escape(si.compiler).c_str(),
                     json_escape(si.flags).c_str(), pinned ? cpu : -1);
        std::fprintf(o,
                     "  \"timer\": {\"name\": \"%s\", \"hz\": %.0f, \"resolution_ns\": %.3f, \"overhead_median_ns\": "
                     "%.3f, \"overhead_min_ns\": %.3f, \"overhead_p99_ns\": %.3f},\n",
                     timer_name(), ticks_per_second(), si.clock_resolution_ns,
                     ticks_to_ns(static_cast<double>(ovh.median_ticks)), ticks_to_ns(static_cast<double>(ovh.min_ticks)),
                     ticks_to_ns(static_cast<double>(ovh.p99_ticks)));
        std::fprintf(o, "  \"allocs_after_warmup_max\": %llu,\n", (unsigned long long)worst_allocs);
        std::fprintf(o, "  \"minor_faults_max\": %ld,\n", worst_faults);
        std::fprintf(o, "  \"summary\": {");
        for (std::size_t i = 0; i < std::size(metrics); ++i) {
            const Spread s = col(metrics[i].m);
            std::fprintf(o, "%s\n    \"%s\": {\"median\": %.3f, \"min\": %.3f, \"max\": %.3f, \"spread\": %.5f}",
                         i ? "," : "", metrics[i].name, s.median, s.min, s.max, s.rel());
        }
        std::fprintf(o, "\n  },\n  \"runs\": [");
        for (std::size_t i = 0; i < res.size(); ++i) {
            const RunResult& r = res[i];
            std::fprintf(o,
                         "%s\n    {\"p50_ns\": %.3f, \"p90_ns\": %.3f, \"p99_ns\": %.3f, \"p999_ns\": %.3f, "
                         "\"p9999_ns\": %.3f, \"max_ns\": %.3f, \"mean_ns\": %.3f, \"book_msgs\": %llu, "
                         "\"allocs_warmup\": %llu, \"allocs_measured\": %llu, \"minor_faults\": %ld, "
                         "\"timed_ns_per_msg\": %.3f, \"untimed_ns_per_msg\": %.3f}",
                         i ? "," : "", r.p50, r.p90, r.p99, r.p999, r.p9999, r.max, r.mean,
                         (unsigned long long)r.book_msgs, (unsigned long long)r.allocs_warmup,
                         (unsigned long long)r.allocs_measured, r.faults_measured, r.timed_ns_per_msg,
                         r.untimed_ns_per_msg);
        }
        std::fprintf(o, "\n  ]\n}\n");
        std::fclose(o);
        std::printf("\n  wrote %s\n", json.c_str());
    }

    if (worst_allocs) {
        std::printf("FAIL: %llu heap allocations after warmup (must be 0)\n", (unsigned long long)worst_allocs);
        return 1;
    }
    std::printf("PASS: zero heap allocations after warmup in all %d runs\n", runs);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    int rc = 2;
    try {
        if (cmd == "slice") rc = cmd_slice(argc, argv);
        else if (cmd == "run") rc = cmd_run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "perfguard: %s\n", e.what());
        return 1;
    }
    if (rc == 2)
        std::fprintf(stderr,
                     "usage: perfguard slice IN OUT [--messages N]\n"
                     "       perfguard run FILE [--warmup N] [--runs N] [--cpu N] [--json OUT]\n");
    return rc;
}
