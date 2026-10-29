// The golden hash file: written once from the file replay through the
// order-level golden model, then every Version 2 stage (network replay,
// recovery, kernel bypass) must reproduce it exactly.
//
// Format (text):
//   # fh-golden-hashes v1
//   # source <file name> <bytes>
//   # every <N>
//   cp <book_msgs> <level_digest> <order_hash>     (one per checkpoint)
//   final <book_msgs> <level_digest> <order_hash>
//
// level_digest chains Version 1's level hash after EVERY message, so equal
// digests mean the aggregated books matched message by message. order_hash
// walks every order in queue order, so equal values mean the queues matched.
#pragma once

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fh/book/book_common.hpp"

namespace fh::golden {

inline uint64_t chain(uint64_t digest, uint64_t level_hash) noexcept { return mix64(digest ^ level_hash); }

struct Checkpoint {
    uint64_t book_msgs = 0, level_digest = 0, order_hash = 0;
    bool operator==(const Checkpoint&) const = default;
};

struct File {
    std::string source;
    uint64_t source_bytes = 0;
    uint64_t every = 0;
    std::vector<Checkpoint> checkpoints;
    Checkpoint final;
};

inline void write(const std::string& path, const File& f) {
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (!out) throw std::runtime_error("cannot write " + path);
    std::fprintf(out, "# fh-golden-hashes v1\n# source %s %" PRIu64 "\n# every %" PRIu64 "\n", f.source.c_str(),
                 f.source_bytes, f.every);
    for (const auto& c : f.checkpoints)
        std::fprintf(out, "cp %" PRIu64 " %016" PRIx64 " %016" PRIx64 "\n", c.book_msgs, c.level_digest, c.order_hash);
    std::fprintf(out, "final %" PRIu64 " %016" PRIx64 " %016" PRIx64 "\n", f.final.book_msgs, f.final.level_digest,
                 f.final.order_hash);
    std::fclose(out);
}

inline File read(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    File f;
    for (std::string line; std::getline(in, line);) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "#") {
            std::string key;
            ss >> key;
            if (key == "source") ss >> f.source >> f.source_bytes;
            if (key == "every") ss >> f.every;
        } else if (tag == "cp" || tag == "final") {
            Checkpoint c;
            ss >> c.book_msgs >> std::hex >> c.level_digest >> c.order_hash;
            (tag == "cp" ? f.checkpoints.emplace_back(c) : f.final) = c;
        }
    }
    return f;
}

}  // namespace fh::golden
