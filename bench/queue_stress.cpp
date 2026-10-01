// Long-run queue stress: billions of operations, every message checked.
//
//   queue_stress [--queue NAME|all] [--capacity 4|64|1024|65536] [--seconds S]
//                [--delays] [--cpu-producer A --cpu-consumer B]
//
// The producer sends 48-byte messages whose every field is derived from a
// sequence number. The consumer checks the sequence is exactly +1 each time
// and that every field matches, so a torn slot, a stale slot (read before the
// producer's write became visible), a lost or a duplicated message is counted.
// --delays inserts random tiny spins on both sides to vary the interleaving.
// Exit status is non-zero if any error was seen.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "fh/queue/queue_kinds.hpp"

using namespace fh;

namespace {

struct Result {
    uint64_t items = 0, seq_errors = 0, payload_errors = 0;
    double seconds = 0;
};

inline void fill(Msg& m, uint64_t i) {
    m.ref = i;
    m.new_ref = ~i;
    m.stamp = i * 0x9E3779B97F4A7C15ull;
    m.timestamp = i ^ 0x5DEECE66Dull;
    m.price = static_cast<uint32_t>(i * 2654435761u);
    m.shares = static_cast<uint32_t>(i >> 7);
    m.locate = static_cast<uint16_t>(i * 31);
    m.flags = static_cast<uint32_t>(i);
}

inline bool intact(const Msg& m, uint64_t i) {
    return m.new_ref == ~i && m.stamp == i * 0x9E3779B97F4A7C15ull && m.timestamp == (i ^ 0x5DEECE66Dull) &&
           m.price == static_cast<uint32_t>(i * 2654435761u) && m.shares == static_cast<uint32_t>(i >> 7) &&
           m.locate == static_cast<uint16_t>(i * 31) && m.flags == static_cast<uint32_t>(i);
}

template <class Q>
Result stress(double seconds, bool delays, int cpu_p, int cpu_c) {
    auto q = std::make_unique<Q>();
    std::atomic<bool> stop{false};
    Result r;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread consumer([&] {
        pin_current_thread(cpu_c);
        std::mt19937 rng(2);
        Msg m;
        uint64_t expect = 0;
        for (;;) {
            q->pop(m);
            if (m.ref == ~uint64_t{0}) break;  // end marker
            if (m.ref != expect) {
                ++r.seq_errors;
                expect = m.ref;  // resynchronise so one error is counted once
            }
            if (!intact(m, m.ref)) ++r.payload_errors;
            ++expect;
            if (delays) {
                const uint32_t x = rng();
                for (uint32_t k = 0, n = x & 0x1F; k < n; ++k) cpu_relax();
            }
        }
        r.items = expect;
    });
    pin_current_thread(cpu_p);
    std::thread timer([&] {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        stop.store(true, std::memory_order_relaxed);
    });
    std::mt19937 rng(1);
    Msg m{};
    uint64_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        for (int burst = 0; burst < 4096; ++burst, ++i) {  // check the clock rarely
            fill(m, i);
            q->push(m);
            if (delays) {
                const uint32_t x = rng();
                for (uint32_t k = 0, n = x & 0x1F; k < n; ++k) cpu_relax();
            }
        }
    }
    m.ref = ~uint64_t{0};
    q->push(m);
    q->flush();
    consumer.join();
    timer.join();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

template <template <class, std::size_t> class Q>
Result by_capacity(std::size_t cap, double s, bool d, int p, int c) {
    switch (cap) {
        case 4: return stress<Q<Msg, 4>>(s, d, p, c);
        case 64: return stress<Q<Msg, 64>>(s, d, p, c);
        case 1024: return stress<Q<Msg, 1024>>(s, d, p, c);
        case 65536: return stress<Q<Msg, 65536>>(s, d, p, c);
        default: throw std::invalid_argument("--capacity must be 4, 64, 1024 or 65536");
    }
}

Result run(const std::string& q, std::size_t cap, double s, bool d, int p, int c) {
    if (q == "spsc") return by_capacity<SpscAligned>(cap, s, d, p, c);
    if (q == "spsc_unaligned") return by_capacity<SpscUnaligned>(cap, s, d, p, c);
    if (q == "mutex") return by_capacity<MutexQueue>(cap, s, d, p, c);
    if (q == "rigtorp") return by_capacity<RigtorpQueue>(cap, s, d, p, c);
#if defined(FH_HAVE_BOOST)
    if (q == "boost") return by_capacity<BoostQueue>(cap, s, d, p, c);
#endif
    throw std::invalid_argument("unknown queue " + q);
}

}  // namespace

int main(int argc, char** argv) {
    std::string queue = "all";
    std::size_t cap = 1024;
    double seconds = 10;
    bool delays = false;
    int cpu_p = -1, cpu_c = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (k == "--queue") queue = next();
        else if (k == "--capacity") cap = std::stoul(next());
        else if (k == "--seconds") seconds = std::stod(next());
        else if (k == "--delays") delays = true;
        else if (k == "--cpu-producer") cpu_p = std::stoi(next());
        else if (k == "--cpu-consumer") cpu_c = std::stoi(next());
    }
#if defined(FH_INJECT_ORDERING_BUG)
    std::printf("NOTE: built with FH_INJECT_ORDERING_BUG (spsc publishes head with a relaxed store)\n");
#endif
    std::vector<std::string> queues = queue == "all" ? queue_names() : std::vector<std::string>{queue};
    bool clean = true;
    std::printf("%-15s %8s %7s %16s %10s %10s %10s\n", "queue", "capacity", "delays", "items", "M ops/s", "seq_err",
                "payload_err");
    for (const auto& q : queues) {
        const Result r = run(q, cap, seconds, delays, cpu_p, cpu_c);
        std::printf("%-15s %8zu %7s %16llu %10.1f %10llu %10llu\n", q.c_str(), cap, delays ? "yes" : "no",
                    (unsigned long long)r.items, static_cast<double>(r.items) / r.seconds / 1e6,
                    (unsigned long long)r.seq_errors, (unsigned long long)r.payload_errors);
        std::fflush(stdout);
        clean = clean && r.seq_errors == 0 && r.payload_errors == 0;
    }
    std::printf("%s\n", clean ? "CLEAN: every message arrived once, in order, intact" : "ERRORS DETECTED");
    return clean ? 0 : 1;
}
