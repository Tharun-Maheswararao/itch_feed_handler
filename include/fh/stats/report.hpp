// Console report and CSV output for pipeline runs.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "fh/pipeline.hpp"
#include "fh/util/sysinfo.hpp"

namespace fh {

struct RunRecord {
    int run = 0;
    std::string label;
    RunResult result;
};

// Message types defined by ITCH 5.0, in spec order.
inline constexpr const char* kItchTypes = "SRHYLVWKJhAFECXDUPQBINO";

double msgs_per_sec(uint64_t messages, double seconds);

void print_message_mix(std::ostream& os, const ParseStats& st);
void print_run(std::ostream& os, const RunRecord& rec);
void print_system(std::ostream& os, const SystemInfo& si);

// Index of the median run: by throughput for unpaced runs (throughput is the
// result), by p99 end-to-end latency for paced runs (throughput is fixed).
std::size_t median_run(const std::vector<RunRecord>& runs);

// Writes <dir>/histogram.csv and <dir>/run_stats.csv.
void write_csvs(const std::string& dir, const std::vector<RunRecord>& runs, const SystemInfo& si);

}  // namespace fh
