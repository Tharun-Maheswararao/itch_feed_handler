#include "fh/view/live_view.hpp"

#include <cstdio>
#include <iostream>

namespace fh {

namespace {
std::string fmt_price(uint32_t p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
    return buf;
}
std::string fmt_time(uint64_t ns) {
    const uint64_t s = ns / 1'000'000'000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%03llu", static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
                  static_cast<unsigned long long>(ns / 1'000'000 % 1000));
    return buf;
}
}  // namespace

std::string render_snapshot(const BookSnapshot& s) {
    std::string sym(s.symbol, 8);
    while (!sym.empty() && sym.back() == ' ') sym.pop_back();
    std::string out;
    char line[160];
    std::snprintf(line, sizeof line, " %-8s  ITCH time %s   book msgs %llu\n", sym.c_str(), fmt_time(s.timestamp).c_str(),
                  static_cast<unsigned long long>(s.messages));
    out += line;
    out += " ---------------------------------------------------------------\n";
    std::snprintf(line, sizeof line, " %6s %10s %12s | %-12s %-10s %-6s\n", "orders", "shares", "bid", "ask", "shares",
                  "orders");
    out += line;
    for (int i = 0; i < BookSnapshot::kDepth; ++i) {
        std::string bid = "", bsh = "", bn = "", ask = "", ash = "", an = "";
        if (i < s.n_bids) {
            bid = fmt_price(s.bids[i].price);
            bsh = std::to_string(s.bids[i].shares);
            bn = std::to_string(s.bids[i].orders);
        }
        if (i < s.n_asks) {
            ask = fmt_price(s.asks[i].price);
            ash = std::to_string(s.asks[i].shares);
            an = std::to_string(s.asks[i].orders);
        }
        std::snprintf(line, sizeof line, " %6s %10s %12s | %-12s %-10s %-6s\n", bn.c_str(), bsh.c_str(), bid.c_str(),
                      ask.c_str(), ash.c_str(), an.c_str());
        out += line;
    }
    if (s.n_bids && s.n_asks) {
        const double spread = (static_cast<double>(s.asks[0].price) - static_cast<double>(s.bids[0].price)) / 1e4;
        std::snprintf(line, sizeof line, " spread %.4f\n", spread);
        out += line;
    }
    return out;
}

LiveView::LiveView(const Seqlock<BookSnapshot>& source, std::chrono::milliseconds interval, std::string record_path,
                   bool draw)
    : source_(source), interval_(interval), record_path_(std::move(record_path)), draw_(draw) {
    if (!record_path_.empty()) record_.open(record_path_);
}

LiveView::~LiveView() { stop(); }

void LiveView::start() {
    thread_ = std::thread([this] {
        auto next = std::chrono::steady_clock::now();
        while (!stop_.load(std::memory_order_relaxed)) {
            next += interval_;
            std::this_thread::sleep_until(next);
            frame();
        }
    });
}

void LiveView::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
    frame();
}

void LiveView::frame() {
    BookSnapshot s;
    if (!source_.load(s)) return;
    const uint64_t v = source_.version();
    if (v == last_version_) return;  // nothing new since the last frame
    last_version_ = v;
    const std::string text = render_snapshot(s);
    if (draw_) std::cout << "\x1b[2J\x1b[H" << text << std::flush;
    if (record_) record_ << text << "\f\n" << std::flush;  // form feed separates frames
    ++frames_;
}

}  // namespace fh
