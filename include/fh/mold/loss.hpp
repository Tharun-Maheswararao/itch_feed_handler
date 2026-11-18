// Seeded loss injection for the replay publisher.
//
// Every decision comes from two random streams per line plus one shared
// stream, and each packet consumes the same number of draws whatever happens
// to it. So the same seed and the same file give the same drops, duplicates
// and reorderings, run after run, and changing one line's settings never
// shifts the other line's decisions. A failing test therefore replays
// exactly.
//
// Decisions are per packet index (0-based, in send order), not per sequence
// number: both lines carry identical packets, so the index identifies the
// same packet on each.
#pragma once

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace fh::mold {

enum class Action : uint8_t { Send, Drop, Duplicate, Delay };

inline const char* action_name(Action a) {
    switch (a) {
        case Action::Send: return "send";
        case Action::Drop: return "drop";
        case Action::Duplicate: return "duplicate";
        case Action::Delay: return "delay";
    }
    return "?";
}

struct LossConfig {
    static constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

    uint64_t seed = 1;
    double drop[2] = {0, 0};  // independent drop probability, line A and line B
    double drop_both = 0;     // the same packet dropped on both lines
    double burst_rate = 0;    // probability that a burst starts at a packet (each line on its own)
    uint32_t burst_len = 0;   // packets lost per random burst
    double duplicate = 0;     // probability a packet is sent twice
    double reorder = 0;       // probability a packet is held back and sent after the next one

    // Packet indices [first, last] dropped on the lines in `lines` (bit 0 =
    // A, bit 1 = B): a deterministic burst for tests ("500 packets on both").
    struct Range {
        uint64_t first, last;
        uint8_t lines;
    };
    std::vector<Range> drop_ranges;

    uint64_t kill_from[2] = {kNever, kNever};  // the line goes silent from this packet index on

    bool any() const {
        return drop[0] > 0 || drop[1] > 0 || drop_both > 0 || (burst_rate > 0 && burst_len > 0) || duplicate > 0 ||
               reorder > 0 || !drop_ranges.empty() || kill_from[0] != kNever || kill_from[1] != kNever;
    }
};

class LossInjector {
public:
    explicit LossInjector(const LossConfig& c)
        : c_(c), line_rng_{std::mt19937_64(c.seed * 2 + 1), std::mt19937_64(c.seed * 2 + 2)},
          both_rng_(c.seed ^ 0x9E3779B97F4A7C15ull) {}

    // A line that is dead sends nothing at all, heartbeats included.
    bool dead(uint64_t packet, int line) const { return packet >= c_.kill_from[line]; }

    // Decide both lines for one packet. Call once per packet, in order.
    void decide(uint64_t packet, Action out[2]) {
        const bool both = uniform(both_rng_) < c_.drop_both;
        for (int l = 0; l < 2; ++l) {
            auto& r = line_rng_[l];
            // Fixed number of draws per packet and line, used or not.
            const double u_drop = uniform(r), u_burst = uniform(r), u_dup = uniform(r), u_delay = uniform(r);
            if (burst_left_[l] == 0 && c_.burst_len > 0 && u_burst < c_.burst_rate) burst_left_[l] = c_.burst_len;
            Action a = Action::Send;
            if (dead(packet, l) || in_range(packet, l) || both) {
                a = Action::Drop;
            } else if (burst_left_[l] > 0) {
                a = Action::Drop;
            } else if (u_drop < c_.drop[l]) {
                a = Action::Drop;
            } else if (u_delay < c_.reorder) {
                a = Action::Delay;
            } else if (u_dup < c_.duplicate) {
                a = Action::Duplicate;
            }
            if (burst_left_[l] > 0) --burst_left_[l];
            out[l] = a;
        }
    }

private:
    static double uniform(std::mt19937_64& r) { return static_cast<double>(r() >> 11) * 0x1.0p-53; }

    bool in_range(uint64_t packet, int line) const {
        for (const auto& rg : c_.drop_ranges)
            if ((rg.lines >> line & 1) && packet >= rg.first && packet <= rg.last) return true;
        return false;
    }

    LossConfig c_;
    std::mt19937_64 line_rng_[2];
    std::mt19937_64 both_rng_;
    uint32_t burst_left_[2] = {0, 0};
};

}  // namespace fh::mold
