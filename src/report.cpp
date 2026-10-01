#include "fh/stats/report.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <ostream>
#include <stdexcept>

namespace fh {

namespace {

struct Pct {
    const char* name;
    double q;
};
constexpr Pct kPcts[] = {{"p50", 0.50}, {"p90", 0.90}, {"p99", 0.99}, {"p999", 0.999}};

std::string fmt_ns(double ns) {
    char buf[32];
    if (ns < 1e3)
        std::snprintf(buf, sizeof buf, "%.0f ns", ns);
    else if (ns < 1e6)
        std::snprintf(buf, sizeof buf, "%.2f us", ns / 1e3);
    else if (ns < 1e9)
        std::snprintf(buf, sizeof buf, "%.2f ms", ns / 1e6);
    else
        std::snprintf(buf, sizeof buf, "%.2f s", ns / 1e9);
    return buf;
}

std::string csv_escape(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) out += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

}  // namespace

double msgs_per_sec(uint64_t messages, double seconds) {
    return seconds > 0 ? static_cast<double>(messages) / seconds : 0.0;
}

void print_message_mix(std::ostream& os, const ParseStats& st) {
    os << "Message mix (" << st.messages << " messages, " << std::fixed << std::setprecision(2)
       << static_cast<double>(st.bytes) / 1e9 << " GB";
    if (st.malformed) os << ", " << st.malformed << " malformed";
    if (st.truncated) os << ", TRUNCATED";
    os << ")\n";
    std::vector<std::pair<uint64_t, int>> rows;
    for (int t = 0; t < 256; ++t)
        if (st.counts[t]) rows.push_back({st.counts[t], t});
    std::sort(rows.rbegin(), rows.rend());
    for (auto [n, t] : rows) {
        os << "  " << static_cast<char>(t) << "  " << std::setw(12) << n << "  " << std::setw(6) << std::setprecision(2)
           << 100.0 * static_cast<double>(n) / static_cast<double>(st.messages) << "%\n";
    }
    os.unsetf(std::ios::floatfield);
}

void print_run(std::ostream& os, const RunRecord& rec) {
    const RunResult& r = rec.result;
    os << "Run " << rec.run << " [" << rec.label << "]" << (r.ring_batch > 1 ? "  ring batch " + std::to_string(r.ring_batch) : std::string()) << "  wall " << std::fixed << std::setprecision(3) << r.wall_seconds
       << " s   " << std::setprecision(2) << msgs_per_sec(r.parse.messages, r.wall_seconds) / 1e6
       << " M file msgs/s   " << msgs_per_sec(r.book_messages, r.wall_seconds) / 1e6 << " M book msgs/s\n";
    os << "  book msgs " << r.book_messages << "   mean book update "
       << std::setprecision(1) << (r.book_messages ? r.consumer_busy_seconds * 1e9 / static_cast<double>(r.book_messages) : 0)
       << " ns (from summed busy time)\n";
    os << "  " << std::left << std::setw(8) << "latency" << std::right;
    for (auto p : kPcts) os << std::setw(11) << p.name;
    os << std::setw(11) << "max" << std::setw(11) << "mean" << "\n";
    auto row = [&](const char* name, const LatencyHistogram& h) {
        os << "  " << std::left << std::setw(8) << name << std::right;
        for (auto p : kPcts) os << std::setw(11) << fmt_ns(ticks_to_ns(static_cast<double>(h.percentile(p.q))));
        os << std::setw(11) << fmt_ns(ticks_to_ns(static_cast<double>(h.max())));
        os << std::setw(11) << fmt_ns(ticks_to_ns(h.mean())) << "\n";
    };
    row("total", r.total);
    row("queue", r.queue);
    row("book", r.book);
    os.unsetf(std::ios::floatfield);
    const auto& c = r.counters;
    os << "  book: adds " << c.adds << "  exec " << c.executes << "  cancel " << c.cancels << "  delete " << c.deletes
       << "  replace " << c.replaces << "  live " << c.live_orders << "  peak " << c.peak_live_orders
       << "  unknown_ref " << c.unknown_ref << "  overfill " << c.overfill << "\n";
}

void print_system(std::ostream& os, const SystemInfo& si) {
    os << "CPU      " << si.cpu_model << " (" << si.physical_cores << " physical / " << si.logical_cores
       << " logical cores)\n"
       << "OS       " << si.os << "\n"
       << "Compiler " << si.compiler << "\n"
       << "Flags    " << si.flags << " [" << si.build_type << "]\n"
       << "Clock    " << si.clock_source << " @ " << std::fixed << std::setprecision(3) << si.clock_hz / 1e6
       << " MHz (" << std::setprecision(2) << 1e9 / si.clock_hz << " ns per tick, measured resolution "
       << si.clock_resolution_ns << " ns)\n";
    os.unsetf(std::ios::floatfield);
}

std::size_t median_run(const std::vector<RunRecord>& runs) {
    std::vector<std::size_t> idx(runs.size());
    std::iota(idx.begin(), idx.end(), 0);
    const bool paced = !runs.empty() && runs[0].result.paced;
    auto key = [&](std::size_t i) {
        const auto& r = runs[i].result;
        return paced ? static_cast<double>(r.total.percentile(0.99)) : msgs_per_sec(r.book_messages, r.wall_seconds);
    };
    std::sort(idx.begin(), idx.end(), [&](auto a, auto b) { return key(a) < key(b); });
    return idx.empty() ? 0 : idx[idx.size() / 2];
}

void write_csvs(const std::string& dir, const std::vector<RunRecord>& runs, const SystemInfo& si) {
    std::filesystem::create_directories(dir);
    const std::size_t med = median_run(runs);

    std::ofstream h(dir + "/histogram.csv");
    if (!h) throw std::runtime_error("cannot write " + dir + "/histogram.csv");
    h << "run,label,median,metric,bucket_lo_ns,bucket_hi_ns,count\n";
    for (std::size_t k = 0; k < runs.size(); ++k) {
        const auto& rec = runs[k];
        const std::pair<const char*, const LatencyHistogram*> metrics[] = {
            {"total", &rec.result.total}, {"queue", &rec.result.queue}, {"book", &rec.result.book}};
        for (auto [name, hist] : metrics) {
            for (std::size_t i = 0; i < LatencyHistogram::kBuckets; ++i) {
                const uint64_t n = hist->bucket_count(i);
                if (!n) continue;
                h << rec.run << ',' << csv_escape(rec.label) << ',' << (k == med) << ',' << name << ','
                  << ticks_to_ns(static_cast<double>(LatencyHistogram::lower_bound(i))) << ','
                  << ticks_to_ns(static_cast<double>(LatencyHistogram::upper_bound(i))) << ',' << n << '\n';
            }
        }
    }

    std::ofstream s(dir + "/run_stats.csv");
    if (!s) throw std::runtime_error("cannot write " + dir + "/run_stats.csv");
    s << "run,label,median,paced,ring_batch,wall_s,file_messages,book_messages,file_msgs_per_sec,book_msgs_per_sec,mean_book_update_ns";
    for (const char* m : {"total", "queue", "book"}) {
        for (auto p : kPcts) s << ',' << m << '_' << p.name << "_ns";
        s << ',' << m << "_max_ns," << m << "_mean_ns";
    }
    for (const char* t = kItchTypes; *t; ++t) s << ",count_" << *t;
    s << ",count_other,peak_live_orders,unknown_ref,producer_pinned,consumer_pinned"
         ",cpu_model,physical_cores,logical_cores,os,compiler,flags,clock_source,clock_ns_per_tick,clock_resolution_ns\n";
    s << std::setprecision(10);
    for (std::size_t k = 0; k < runs.size(); ++k) {
        const auto& rec = runs[k];
        const RunResult& r = rec.result;
        s << rec.run << ',' << csv_escape(rec.label) << ',' << (k == med) << ',' << r.paced << ','
          << r.ring_batch << ',' << r.wall_seconds << ',' << r.parse.messages << ',' << r.book_messages << ',' << msgs_per_sec(r.parse.messages, r.wall_seconds)
          << ',' << msgs_per_sec(r.book_messages, r.wall_seconds) << ','
          << (r.book_messages ? r.consumer_busy_seconds * 1e9 / static_cast<double>(r.book_messages) : 0);
        for (const LatencyHistogram* hist : {&r.total, &r.queue, &r.book}) {
            for (auto p : kPcts) s << ',' << ticks_to_ns(static_cast<double>(hist->percentile(p.q)));
            s << ',' << ticks_to_ns(static_cast<double>(hist->max())) << ',' << ticks_to_ns(hist->mean());
        }
        uint64_t known = 0;
        for (const char* t = kItchTypes; *t; ++t) {
            s << ',' << r.parse.count(*t);
            known += r.parse.count(*t);
        }
        s << ',' << (r.parse.messages - known) << ',' << r.counters.peak_live_orders << ',' << r.counters.unknown_ref
          << ',' << r.producer_pinned << ',' << r.consumer_pinned << ',' << csv_escape(si.cpu_model) << ','
          << si.physical_cores << ',' << si.logical_cores << ',' << csv_escape(si.os) << ','
          << csv_escape(si.compiler) << ',' << csv_escape(si.flags) << ',' << si.clock_source << ','
          << 1e9 / si.clock_hz << ',' << si.clock_resolution_ns << '\n';
    }
}

}  // namespace fh
