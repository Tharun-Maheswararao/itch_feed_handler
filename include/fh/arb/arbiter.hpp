// Line arbitration, gap detection and the reorder buffer (Stages C2, C3).
//
// One number drives everything: `expected`, the next sequence number the book
// needs. Every packet from line A, line B or recovery goes through
// on_packet(), and only messages at `expected` are delivered, so the sink sees
// each message exactly once, in sequence order, whichever line carried it.
//
//   packet range [s, e) vs expected    action
//   e <= expected                       duplicate (usually the other line): drop
//   s <  expected < e                   overlap: skip what was applied, deliver the rest
//   s == expected                       deliver all, advance
//   s >  expected                       gap: copy into the reorder buffer
//
// After delivering, the buffer is drained while it holds the packet that
// starts at (or overlaps) `expected`.
//
// Reorder buffer: fixed slots holding packet copies, found through an index
// by sequence number modulo a message window. A packet that starts beyond
// the window, or arrives when every slot is taken, is not stored (counted as
// overflow): the missing range then extends to the highest sequence seen, and
// recovery fetches all of it. Nothing allocates after construction.
//
// "highest" is one past the highest message known to exist: a data packet's
// end, a heartbeat's sequence (the next message to be sent) or the end of
// session's. A gap is simply expected < highest.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "fh/mold/moldudp64.hpp"

namespace fh::arb {

inline constexpr uint8_t kLineRecovery = 2;

struct ArbiterConfig {
    uint32_t buffer_packets = 4096;      // reorder buffer slots
    uint64_t window_messages = 1 << 20;  // power of two; a stored packet must start below expected + window
};

struct ArbStats {
    uint64_t packets[3] = {0, 0, 0};  // received per line: A, B, recovery
    uint64_t heartbeats = 0, ends = 0;
    uint64_t applied = 0;      // packets that delivered at least one message
    uint64_t messages = 0;     // messages delivered
    uint64_t duplicates = 0;   // every message already delivered
    uint64_t overlaps = 0;     // older messages skipped, the rest delivered
    uint64_t buffered = 0;     // stored in the reorder buffer
    uint64_t buffer_dups = 0;  // the same packet was already stored
    uint64_t overflow = 0;     // did not fit in the buffer: recovered instead
    uint64_t stale = 0;        // stored packets made obsolete before use
    uint64_t bad_session = 0, malformed = 0;
    uint64_t gaps = 0;         // gap episodes
    uint64_t resets = 0;       // snapshot loads
};

// Sink requirements:
//   void on_message(uint64_t seq, const uint8_t* msg, uint16_t len, uint64_t rx_ns, uint8_t line);
//   void gap_closed(uint64_t opened_ns, uint64_t closed_ns);
template <class Sink>
class Arbiter {
public:
    Arbiter(Sink& sink, const ArbiterConfig& c = {})
        : sink_(sink), slots_(c.buffer_packets), start_at_(c.window_messages, 0), window_(c.window_messages) {
        if (window_ < 1024 || (window_ & (window_ - 1)) != 0)
            throw std::invalid_argument("arbiter window must be a power of two >= 1024");
        if (c.buffer_packets == 0) throw std::invalid_argument("arbiter needs at least one buffer slot");
        free_.reserve(slots_.size());
        for (uint32_t i = static_cast<uint32_t>(slots_.size()); i > 0; --i) free_.push_back(i - 1);
    }

    void on_packet(const uint8_t* data, uint32_t len, uint8_t line, uint64_t rx_ns) {
        mold::Header h;
        if (!mold::read_header(data, len, h)) {
            ++st_.malformed;
            return;
        }
        if (!has_session_) {
            session_ = h.session;
            has_session_ = true;
        } else if (!(h.session == session_)) {
            ++st_.bad_session;
            return;
        }
        ++st_.packets[line < 3 ? line : 2];
        if (h.heartbeat()) {
            ++st_.heartbeats;
            seen(h.seq);
        } else if (h.end_of_session()) {
            ++st_.ends;
            end_seq_ = h.seq;
            seen(h.seq);
        } else if (!mold::validate(data, len, h)) {
            ++st_.malformed;
        } else {
            seen(h.end());
            if (h.end() <= expected_) {
                ++st_.duplicates;
            } else if (h.seq <= expected_) {
                if (h.seq < expected_) ++st_.overlaps;
                deliver(data, len, h, line, rx_ns);
                drain();
            } else {
                store(data, len, h, line, rx_ns);
            }
        }
        track_gap(rx_ns);
    }

    // A snapshot made the book whole up to next - 1: continue from `next`.
    void reset_to(uint64_t next, uint64_t now_ns) {
        ++st_.resets;
        if (next > expected_) expected_ = next;
        seen(expected_);
        drain();
        sweep_stale();
        track_gap(now_ns);
    }

    uint64_t expected() const { return expected_; }
    uint64_t highest() const { return highest_; }
    bool in_gap() const { return highest_ > expected_; }
    uint64_t gap_opened_ns() const { return gap_since_; }
    uint64_t end_seq() const { return end_seq_; }
    bool done() const { return end_seq_ != 0 && expected_ >= end_seq_; }
    uint32_t buffered_now() const { return used_; }
    const ArbStats& stats() const { return st_; }
    bool has_session() const { return has_session_; }
    const mold::Session& session() const { return session_; }

    // Exclusive end of the missing range that starts at expected: the first
    // stored packet after it, or `highest` if nothing is stored or the buffer
    // overflowed during this gap (then the stored packets are not the only
    // data that is missing). Scans the slots: call on a timer, not per packet.
    uint64_t gap_end() const {
        if (!in_gap()) return expected_;
        if (overflowed_ || used_ == 0) return highest_;
        uint64_t m = highest_;
        for (const Slot& s : slots_)
            if (s.used && s.seq > expected_) m = std::min(m, s.seq);
        return m;
    }

private:
    struct Slot {
        uint64_t seq = 0;
        uint64_t rx_ns = 0;
        uint16_t count = 0;
        uint16_t len = 0;
        uint8_t line = 0;
        bool used = false;
        uint8_t data[mold::kMaxDatagram];
    };

    void seen(uint64_t end) {
        if (end > highest_) highest_ = end;
    }

    void track_gap(uint64_t now_ns) {
        const bool g = in_gap();
        if (g && !gap_open_) {
            gap_open_ = true;
            gap_since_ = now_ns;
            ++st_.gaps;
        } else if (!g && gap_open_) {
            gap_open_ = false;
            overflowed_ = false;
            sink_.gap_closed(gap_since_, now_ns);
        }
    }

    void deliver(const uint8_t* data, uint32_t len, const mold::Header& h, uint8_t line, uint64_t rx_ns) {
        mold::for_each_message(data, len, h, [&](uint64_t seq, const uint8_t* m, uint16_t mlen) {
            if (seq < expected_) return;
            sink_.on_message(seq, m, mlen, rx_ns, line);
            ++st_.messages;
        });
        expected_ = h.end();
        ++st_.applied;
    }

    uint32_t lookup(uint64_t seq) const {
        const uint32_t v = start_at_[seq & (window_ - 1)];
        return v && slots_[v - 1].used && slots_[v - 1].seq == seq ? v : 0;
    }

    void release(uint32_t v) {
        Slot& s = slots_[v - 1];
        s.used = false;
        start_at_[s.seq & (window_ - 1)] = 0;
        free_.push_back(v - 1);
        --used_;
    }

    void store(const uint8_t* data, uint32_t len, const mold::Header& h, uint8_t line, uint64_t rx_ns) {
        if (h.seq >= expected_ + window_ || len > mold::kMaxDatagram) {
            ++st_.overflow;
            overflowed_ = true;
            return;
        }
        uint32_t& idx = start_at_[h.seq & (window_ - 1)];
        if (idx && slots_[idx - 1].used) {
            if (slots_[idx - 1].seq == h.seq) {
                // Same start. Keep the packet that reaches further: lines
                // that pack differently can start together and end apart.
                ++st_.buffer_dups;
                if (h.count <= slots_[idx - 1].count) return;
            } else {
                // Same index, different start: inside the window that can
                // only be a packet already behind expected.
                ++st_.stale;
            }
            release(idx);
        }
        if (free_.empty()) sweep_stale();
        if (free_.empty()) {
            ++st_.overflow;
            overflowed_ = true;
            return;
        }
        const uint32_t i = free_.back();
        free_.pop_back();
        Slot& s = slots_[i];
        s.seq = h.seq;
        s.count = h.count;
        s.len = static_cast<uint16_t>(len);
        s.line = line;
        s.rx_ns = rx_ns;
        s.used = true;
        std::memcpy(s.data, data, len);
        start_at_[h.seq & (window_ - 1)] = i + 1;
        ++used_;
        ++st_.buffered;
        max_count_ = std::max<uint64_t>(max_count_, h.count);
    }

    void drain() {
        while (used_ > 0) {
            uint32_t v = lookup(expected_);
            if (!v) {
                // A packet that starts before expected but reaches past it
                // (lines that pack messages differently).
                for (uint64_t d = 1; d <= max_count_ && d < expected_ && !v; ++d) {
                    const uint32_t c = lookup(expected_ - d);
                    if (c && slots_[c - 1].seq + slots_[c - 1].count > expected_) v = c;
                }
                if (!v) break;
                ++st_.overlaps;
            }
            Slot& s = slots_[v - 1];
            mold::Header h;
            mold::read_header(s.data, s.len, h);
            deliver(s.data, s.len, h, s.line, s.rx_ns);
            release(v);
        }
        if (used_ > 0 && !in_gap()) sweep_stale();
    }

    void sweep_stale() {
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.used && s.seq + s.count <= expected_) {
                ++st_.stale;
                release(i + 1);
            }
        }
    }

    Sink& sink_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::vector<uint32_t> start_at_;  // slot + 1 by first sequence mod window, 0 = none
    uint64_t window_;
    uint64_t expected_ = 1;
    uint64_t highest_ = 1;
    uint64_t end_seq_ = 0;
    uint64_t max_count_ = 0;
    uint64_t gap_since_ = 0;
    uint32_t used_ = 0;
    bool gap_open_ = false;
    bool overflowed_ = false;
    bool has_session_ = false;
    mold::Session session_{};
    ArbStats st_;
};

}  // namespace fh::arb
