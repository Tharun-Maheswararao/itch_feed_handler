// golden_hashes FILE OUT [--every N]
//
// Replays the ITCH file through the order-level golden model
// (GoldenFifoBook) and writes the checkpoint hashes every later Version 2
// stage must reproduce. See golden_hashes.hpp for the format.
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "fh/book/apply.hpp"
#include "fh/book/golden_fifo_book.hpp"
#include "fh/parser/mapped_file.hpp"
#include "fh/util/platform.hpp"
#include "fh/parser/parser.hpp"
#include "golden_hashes.hpp"

using namespace fh;

int main(int argc, char** argv) {
    // Linux: no pinning here (single thread). macOS: ask for a performance core
    // (QoS USER_INTERACTIVE); on battery, default-QoS work lands on E-cores.
    pin_current_thread(-1);
    if (argc < 3) {
        std::fprintf(stderr, "usage: golden_hashes ITCH_FILE OUT [--every N]\n");
        return 2;
    }
    uint64_t every = uint64_t{1} << 20;
    for (int i = 3; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--every") every = std::stoull(argv[i + 1]);

    MappedFile f(argv[1]);
    auto book = std::make_unique<GoldenFifoBook<true>>();
    golden::File out;
    std::string name = argv[1];
    out.source = name.substr(name.find_last_of('/') + 1);
    out.source_bytes = f.size();
    out.every = every;

    uint64_t n = 0, digest = 0;
    const auto t0 = std::chrono::steady_clock::now();
    parse_buffer(f.data(), f.end(), [&](const Msg& m) {
        apply(*book, m);
        digest = golden::chain(digest, book->hash());
        if (++n % every == 0) out.checkpoints.push_back({n, digest, book->order_hash()});
    });
    out.final = {n, digest, book->order_hash()};
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::string err;
    const bool ok = book->check_invariants(&err);
    golden::write(argv[2], out);
    std::printf("golden model: %llu book msgs in %.1f s, %zu checkpoints, peak live orders %llu, invariants %s\n",
                (unsigned long long)n, secs, out.checkpoints.size(),
                (unsigned long long)book->counters().peak_live_orders, ok ? "OK" : err.c_str());
    std::printf("final level digest %016llx, order hash %016llx -> %s\n", (unsigned long long)digest,
                (unsigned long long)out.final.order_hash, argv[2]);
    return ok ? 0 : 1;
}
