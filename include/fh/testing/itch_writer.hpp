// ITCH 5.0 encoder and a random-but-valid synthetic feed generator.
// Used by the unit tests and by `feed_handler gen` so CI can run the full
// pipeline-vs-golden comparison without the multi-GB Nasdaq file.
#pragma once

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "fh/parser/endian.hpp"

namespace fh::synth {

class ItchWriter {
public:
    std::vector<uint8_t>& bytes() { return buf_; }

    void system_event(uint64_t ts, char code) {
        uint8_t* m = frame(12, 'S', 0, ts);
        m[11] = static_cast<uint8_t>(code);
    }
    void stock_directory(uint16_t locate, uint64_t ts, const std::string& sym) {
        uint8_t* m = frame(39, 'R', locate, ts);
        put_alpha(m + 11, sym, 8);
        m[19] = 'Q';  // market category
        m[20] = 'N';  // financial status
        store_be32(m + 21, 100);  // round lot size
        m[25] = 'N';              // round lots only
        m[26] = 'C';              // issue classification
        m[27] = 'Z';              // issue sub-type (2)
        m[28] = ' ';
        m[29] = 'P';              // authenticity
        m[30] = 'N';              // short sale threshold indicator
        m[31] = 'N';              // IPO flag
        m[32] = '1';              // LULD reference price tier
        m[33] = 'N';              // ETP flag
        store_be32(m + 34, 0);    // ETP leverage factor
        m[38] = 'N';              // inverse indicator
    }
    void add(uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares, const std::string& sym,
             uint32_t price) {
        uint8_t* m = frame(36, 'A', locate, ts);
        add_body(m, ref, side, shares, sym, price);
    }
    void add_mpid(uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares, const std::string& sym,
                  uint32_t price, const std::string& mpid) {
        uint8_t* m = frame(40, 'F', locate, ts);
        add_body(m, ref, side, shares, sym, price);
        put_alpha(m + 36, mpid, 4);
    }
    void executed(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares, uint64_t match) {
        uint8_t* m = frame(31, 'E', locate, ts);
        store_be64(m + 11, ref);
        store_be32(m + 19, shares);
        store_be64(m + 23, match);
    }
    void executed_price(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares, uint64_t match, char printable,
                        uint32_t price) {
        uint8_t* m = frame(36, 'C', locate, ts);
        store_be64(m + 11, ref);
        store_be32(m + 19, shares);
        store_be64(m + 23, match);
        m[31] = static_cast<uint8_t>(printable);
        store_be32(m + 32, price);
    }
    void cancel(uint16_t locate, uint64_t ts, uint64_t ref, uint32_t shares) {
        uint8_t* m = frame(23, 'X', locate, ts);
        store_be64(m + 11, ref);
        store_be32(m + 19, shares);
    }
    void del(uint16_t locate, uint64_t ts, uint64_t ref) {
        uint8_t* m = frame(19, 'D', locate, ts);
        store_be64(m + 11, ref);
    }
    void replace(uint16_t locate, uint64_t ts, uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
        uint8_t* m = frame(35, 'U', locate, ts);
        store_be64(m + 11, old_ref);
        store_be64(m + 19, new_ref);
        store_be32(m + 27, shares);
        store_be32(m + 31, price);
    }
    // A non-book message (Trade 'P', 44 bytes) to exercise the skip path.
    void trade(uint16_t locate, uint64_t ts, uint64_t ref, char side, uint32_t shares, const std::string& sym,
               uint32_t price, uint64_t match) {
        uint8_t* m = frame(44, 'P', locate, ts);
        store_be64(m + 11, ref);
        m[19] = static_cast<uint8_t>(side);
        store_be32(m + 20, shares);
        put_alpha(m + 24, sym, 8);
        store_be32(m + 32, price);
        store_be64(m + 36, match);
    }
    // Arbitrary message of a given type and length (zero-filled body).
    void raw(char type, uint16_t len, uint16_t locate = 0, uint64_t ts = 0) { frame(len, type, locate, ts); }

private:
    uint8_t* frame(uint16_t len, char type, uint16_t locate, uint64_t ts) {
        const std::size_t at = buf_.size();
        buf_.resize(at + 2 + len, 0);
        store_be16(&buf_[at], len);
        uint8_t* m = &buf_[at + 2];
        m[0] = static_cast<uint8_t>(type);
        if (len >= 11) {
            store_be16(m + 1, locate);
            store_be16(m + 3, 0);  // tracking number
            store_be48(m + 5, ts);
        }
        return m;
    }
    static void put_alpha(uint8_t* p, const std::string& s, std::size_t n) {
        std::memset(p, ' ', n);
        std::memcpy(p, s.data(), std::min(s.size(), n));
    }
    static void add_body(uint8_t* m, uint64_t ref, char side, uint32_t shares, const std::string& sym, uint32_t price) {
        store_be64(m + 11, ref);
        m[19] = static_cast<uint8_t>(side);
        store_be32(m + 20, shares);
        put_alpha(m + 24, sym, 8);
        store_be32(m + 32, price);
    }
    std::vector<uint8_t> buf_;
};

// Random walk market over `stocks` symbols: every modify targets a live order,
// so the feed is valid, with a small share of deliberately odd events
// (unknown references, crossed replaces) that both books must agree on.
inline std::vector<uint8_t> synthetic_feed(uint64_t events, uint32_t seed = 42, int stocks = 16) {
    ItchWriter w;
    std::mt19937_64 rng(seed);
    auto uni = [&](uint64_t lo, uint64_t hi) { return std::uniform_int_distribution<uint64_t>(lo, hi)(rng); };

    struct Live {
        uint64_t ref;
        uint16_t locate;
        char side;
        uint32_t shares;
    };
    std::vector<Live> live;
    std::unordered_map<uint64_t, std::size_t> pos;  // ref -> index in live
    std::vector<uint32_t> mid(static_cast<std::size_t>(stocks) + 1, 0);
    std::vector<std::string> sym(static_cast<std::size_t>(stocks) + 1);

    uint64_t ts = 4ull * 3600 * 1'000'000'000;  // 04:00
    uint64_t next_ref = 1, match = 1;
    w.system_event(ts, 'O');
    for (int s = 1; s <= stocks; ++s) {
        sym[s] = "SYM" + std::to_string(s);
        mid[s] = static_cast<uint32_t>(uni(20, 500)) * 10000;
        w.stock_directory(static_cast<uint16_t>(s), ts, sym[s]);
    }
    auto drop = [&](std::size_t i) {
        pos.erase(live[i].ref);
        if (i != live.size() - 1) {
            live[i] = live.back();
            pos[live[i].ref] = i;
        }
        live.pop_back();
    };
    auto pick_price = [&](uint16_t loc, char side) {
        if (uni(0, 19) == 0) mid[loc] = std::max<uint32_t>(10000, mid[loc] + static_cast<uint32_t>(uni(0, 20)) * 100 - 1000);
        const uint32_t off = static_cast<uint32_t>(uni(0, 30)) * 100;  // up to 30 ticks from mid
        return side == 'B' ? mid[loc] - 100 - off : mid[loc] + 100 + off;
    };

    for (uint64_t e = 0; e < events; ++e) {
        ts += uni(1, 5000);
        const uint64_t roll = uni(0, 999);
        if (live.empty() || roll < 330) {
            const auto loc = static_cast<uint16_t>(uni(1, static_cast<uint64_t>(stocks)));
            const char side = uni(0, 1) ? 'B' : 'S';
            const auto shares = static_cast<uint32_t>(uni(1, 10) * 100);
            const uint64_t ref = next_ref++;
            if (roll % 10 == 0)
                w.add_mpid(loc, ts, ref, side, shares, sym[loc], pick_price(loc, side), "MPID");
            else
                w.add(loc, ts, ref, side, shares, sym[loc], pick_price(loc, side));
            pos[ref] = live.size();
            live.push_back({ref, loc, side, shares});
            continue;
        }
        const std::size_t i = uni(0, live.size() - 1);
        Live& o = live[i];
        if (roll < 450) {  // execute, partial or full
            const auto n = static_cast<uint32_t>(uni(1, o.shares));
            if (roll < 420)
                w.executed(o.locate, ts, o.ref, n, match++);
            else
                w.executed_price(o.locate, ts, o.ref, n, match++, 'Y', mid[o.locate]);
            o.shares -= n;
            if (o.shares == 0) drop(i);
        } else if (roll < 560) {  // partial cancel
            if (o.shares < 2) continue;
            const auto n = static_cast<uint32_t>(uni(1, o.shares - 1));
            w.cancel(o.locate, ts, o.ref, n);
            o.shares -= n;
        } else if (roll < 820) {
            w.del(o.locate, ts, o.ref);
            drop(i);
        } else if (roll < 960) {  // replace; sometimes priced through the other side
            const uint64_t nref = next_ref++;
            const auto shares = static_cast<uint32_t>(uni(1, 10) * 100);
            const char other = o.side == 'B' ? 'S' : 'B';
            const uint32_t px = uni(0, 9) == 0 ? pick_price(o.locate, other) : pick_price(o.locate, o.side);
            w.replace(o.locate, ts, o.ref, nref, shares, px);
            const Live moved{nref, o.locate, o.side, shares};
            drop(i);
            pos[nref] = live.size();
            live.push_back(moved);
        } else if (roll < 995) {  // non-book traffic the parser must skip
            w.trade(o.locate, ts, 0, 'B', 100, sym[o.locate], mid[o.locate], match++);
            if (roll % 3 == 0) w.raw('I', 50, o.locate, ts);
        } else {  // modify for a reference that does not exist
            w.del(o.locate, ts, next_ref + 1'000'000'000ull);
        }
    }
    w.system_event(ts + 1, 'C');
    return std::move(w.bytes());
}

}  // namespace fh::synth
