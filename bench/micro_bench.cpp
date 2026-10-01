// Component micro-benchmarks, single threaded unless noted:
//   parse only, parse + golden book, parse + fast book, SPSC queue transfer.
//   micro_bench FILE [--repeat 3] [--cpu-producer A --cpu-consumer B]
//   The SPSC transfer is run at ring batch 1, 8 and 32.
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "fh/book/fast_book.hpp"
#include "fh/book/golden_book.hpp"
#include "fh/parser/mapped_file.hpp"
#include "fh/pipeline.hpp"

using namespace fh;
using clk = std::chrono::steady_clock;

template <class F>
double best_of(int repeat, F&& f) {
    double best = 1e300;
    for (int i = 0; i < repeat; ++i) {
        const auto t0 = clk::now();
        f();
        best = std::min(best, std::chrono::duration<double>(clk::now() - t0).count());
    }
    return best;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: micro_bench FILE [--repeat N]\n");
        return 2;
    }
    int repeat = 3, cpu_p = -1, cpu_c = -1;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--repeat") repeat = std::atoi(argv[i + 1]);
        if (k == "--cpu-producer") cpu_p = std::atoi(argv[i + 1]);
        if (k == "--cpu-consumer") cpu_c = std::atoi(argv[i + 1]);
    }
    MappedFile f(argv[1]);
    f.prefault();

    uint64_t book_msgs = 0, sink = 0;
    const ParseStats st = parse_buffer(f.data(), f.end(), [&](const Msg&) { ++book_msgs; });
    std::printf("%llu messages, %llu book messages, best of %d\n", (unsigned long long)st.messages,
                (unsigned long long)book_msgs, repeat);

    const double t_parse = best_of(repeat, [&] {
        parse_buffer(f.data(), f.end(), [&](const Msg& m) { sink += m.ref; });
    });
    std::printf("%-28s %8.3f s  %7.1f ns/book msg  %7.1f M msgs/s\n", "parse only", t_parse,
                t_parse * 1e9 / book_msgs, st.messages / t_parse / 1e6);

    auto bench_book = [&](const char* name, auto make) {
        const double t = best_of(repeat, [&] {
            auto b = make();
            run_single(f.data(), f.end(), *b, false);
        });
        std::printf("%-28s %8.3f s  %7.1f ns/book msg  (book only ~%.1f ns)\n", name, t, t * 1e9 / book_msgs,
                    (t - t_parse) * 1e9 / book_msgs);
        return t;
    };
    const double tg = bench_book("parse + golden book", [] { return std::make_unique<GoldenBook<false>>(); });
    const double tf = bench_book("parse + fast book", [] { return std::make_unique<FastBook<false>>(); });
    std::printf("fast book speedup over golden (book part): %.2fx\n", (tg - t_parse) / (tf - t_parse));

    // SPSC: raw transfer rate of 48-byte messages between two threads, at
    // several index-publication batch sizes.
    constexpr uint64_t kN = 200'000'000;
    for (std::size_t batch : {std::size_t{1}, std::size_t{8}, std::size_t{32}}) {
        auto q = std::make_unique<MsgQueue>();
        q->set_batch(batch);
        bool pinned_p = false, pinned_c = false;
        const double tq = best_of(1, [&] {
            std::thread c([&] {
                pinned_c = pin_current_thread(cpu_c);
                Msg m;
                for (uint64_t i = 0; i < kN; ++i) {
                    q->pop(m);
                    sink += m.ref;
                }
            });
            pinned_p = pin_current_thread(cpu_p);
            Msg m{};
            for (uint64_t i = 0; i < kN; ++i) {
                m.ref = i;
                q->push(m);
            }
            q->flush();
            c.join();
        });
        char name[64];
        std::snprintf(name, sizeof name, "spsc transfer, batch %zu%s", batch, pinned_p && pinned_c ? ", pinned" : "");
        std::printf("%-28s %8.3f s  %7.2f ns/msg  %7.1f M msgs/s\n", name, tq, tq * 1e9 / kN, kN / tq / 1e6);
    }
    std::printf("(checksum %llx)\n", (unsigned long long)sink);
    return 0;
}
