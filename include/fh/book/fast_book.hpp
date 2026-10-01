// Optimized order book. Same interface and semantics as GoldenBook.
//
//  * Orders: preallocated open-addressing OrderMap.
//  * Levels: one sorted std::vector per side, ordered worst -> best, so the
//    touch is at the back. Almost all activity is near the touch, so a short
//    linear scan from the back finds the level, and inserting or erasing
//    there moves only a few elements. Deep levels fall back to binary search.
//  * Both sides share one ordering by storing a key: price for bids,
//    ~price for asks, so "ascending key" always means "towards the touch".
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "fh/book/book_common.hpp"
#include "fh/book/order_map.hpp"

namespace fh {

template <bool kTrackHash = true>
class FastBook {
public:
    explicit FastBook(std::size_t order_capacity = std::size_t{1} << 23)
        : orders_(order_capacity), stocks_(kMaxLocates), symbols_(kMaxLocates) {}

    void directory(uint16_t locate, const char (&sym)[8]) {
        ++ctr_.directory;
        std::memcpy(symbols_[locate].data(), sym, 8);
    }

    void add(uint64_t ref, uint16_t locate, Side side, uint32_t price, uint32_t shares) {
        ++ctr_.adds;
        bool inserted;
        OrderEntry* e = orders_.insert(ref, inserted);
        if (!inserted) [[unlikely]] {
            ++ctr_.duplicate_ref;
            level_sub(*e, e->shares, true);
        } else {
            if (++ctr_.live_orders > ctr_.peak_live_orders) ctr_.peak_live_orders = ctr_.live_orders;
        }
        e->price = price;
        e->shares = shares;
        e->locate = locate;
        e->side = side;
        level_add(locate, side, price, shares);
    }

    void execute(uint64_t ref, uint32_t shares) {
        ++ctr_.executes;
        reduce(ref, shares);
    }

    void cancel(uint64_t ref, uint32_t shares) {
        ++ctr_.cancels;
        reduce(ref, shares);
    }

    void remove(uint64_t ref) {
        ++ctr_.deletes;
        OrderEntry* e = orders_.find(ref);
        if (!e) [[unlikely]] {
            ++ctr_.unknown_ref;
            return;
        }
        level_sub(*e, e->shares, true);
        orders_.erase(e);
        --ctr_.live_orders;
    }

    void replace(uint64_t old_ref, uint64_t new_ref, uint32_t price, uint32_t shares) {
        ++ctr_.replaces;
        OrderEntry* e = orders_.find(old_ref);
        if (!e) [[unlikely]] {
            ++ctr_.unknown_ref;
            return;
        }
        const uint16_t locate = e->locate;
        const Side side = e->side;
        level_sub(*e, e->shares, true);
        orders_.erase(e);
        --ctr_.live_orders;
        --ctr_.adds;
        add(new_ref, locate, side, price, shares);
    }

    uint64_t hash() const { return hash_; }
    const BookCounters& counters() const { return ctr_; }
    std::string symbol(uint16_t locate) const { return trim_symbol(symbols_[locate].raw); }
    std::size_t order_count() const { return orders_.size(); }
    uint64_t order_map_grows() const { return orders_.grows(); }

    // Best n levels, best first.
    std::vector<LevelView> top(uint16_t locate, Side side, std::size_t n) const {
        std::vector<LevelView> out;
        top_into(locate, side, n, [&](const LevelView& lv) { out.push_back(lv); });
        return out;
    }

    // Allocation-free variant used by the live-view snapshot.
    template <class F>
    void top_into(uint16_t locate, Side side, std::size_t n, F&& f) const {
        const auto& v = levels(locate, side);
        for (std::size_t i = v.size(); i > 0 && n > 0; --i, --n) {
            const Level& l = v[i - 1];
            f(LevelView{price_of(l.key, side), l.orders, l.shares});
        }
    }

    std::size_t depth(uint16_t locate, Side side) const { return levels(locate, side).size(); }

    bool check_invariants(std::string* err = nullptr) const {
        struct Agg {
            uint64_t shares = 0;
            uint32_t orders = 0;
        };
        // Rebuild aggregates per stock from the order map, then compare.
        std::vector<std::vector<std::pair<uint32_t, Agg>>> bid(kMaxLocates), ask(kMaxLocates);
        std::size_t n = 0;
        bool ok = true;
        orders_.for_each([&](const OrderEntry& o) {
            ++n;
            if (o.shares == 0) ok = false;
            auto& vec = (o.side == Side::Buy ? bid : ask)[o.locate];
            vec.push_back({o.price, Agg{o.shares, 1}});
        });
        if (!ok) return fail(err, "order with zero shares");
        if (n != ctr_.live_orders) return fail(err, "live order counter mismatch");
        for (std::size_t loc = 0; loc < kMaxLocates; ++loc) {
            for (Side s : {Side::Buy, Side::Sell}) {
                auto& vec = (s == Side::Buy ? bid : ask)[loc];
                std::sort(vec.begin(), vec.end(), [](auto& a, auto& b) { return a.first < b.first; });
                std::vector<LevelView> want;
                for (auto& [px, a] : vec) {
                    if (!want.empty() && want.back().price == px) {
                        want.back().shares += a.shares;
                        want.back().orders += a.orders;
                    } else {
                        want.push_back({px, a.orders, a.shares});
                    }
                }
                if (s == Side::Buy) std::reverse(want.begin(), want.end());  // best first
                const auto have = top(static_cast<uint16_t>(loc), s, SIZE_MAX);
                if (have != want)
                    return fail(err, "level aggregates differ from orders for locate " + std::to_string(loc));
                for (std::size_t i = 1; i < have.size(); ++i) {
                    const bool sorted = s == Side::Buy ? have[i - 1].price > have[i].price
                                                       : have[i - 1].price < have[i].price;
                    if (!sorted) return fail(err, "levels not strictly sorted");
                }
            }
        }
        return true;
    }

private:
    struct Level {
        uint32_t key;  // price for bids, ~price for asks
        uint32_t orders;
        uint64_t shares;
    };
    struct Stock {
        std::vector<Level> bids, asks;  // worst -> best (touch at back)
    };
    struct Symbol {
        char raw[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        char* data() { return raw; }
    };

    static bool fail(std::string* err, std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    }
    static uint32_t key_of(uint32_t price, Side side) { return side == Side::Buy ? price : ~price; }
    static uint32_t price_of(uint32_t key, Side side) { return side == Side::Buy ? key : ~key; }

    std::vector<Level>& levels(uint16_t locate, Side side) {
        return side == Side::Buy ? stocks_[locate].bids : stocks_[locate].asks;
    }
    const std::vector<Level>& levels(uint16_t locate, Side side) const {
        return side == Side::Buy ? stocks_[locate].bids : stocks_[locate].asks;
    }

    // Index of the level with `key` (found=true) or the index to insert it.
    static std::size_t search(const std::vector<Level>& v, uint32_t key, bool& found) {
        constexpr std::size_t kLinear = 8;
        const std::size_t n = v.size();
        const std::size_t stop = n > kLinear ? n - kLinear : 0;
        std::size_t i = n;
        while (i > stop && v[i - 1].key > key) --i;
        if (i > 0 && v[i - 1].key > key) {  // still deeper: binary search the rest
            i = static_cast<std::size_t>(
                std::lower_bound(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), key,
                                 [](const Level& l, uint32_t k) { return l.key < k; }) -
                v.begin());
            found = i < n && v[i].key == key;
            return i;
        }
        if (i > 0 && v[i - 1].key == key) {
            found = true;
            return i - 1;
        }
        found = false;
        return i;
    }

    void touch(uint16_t locate, Side side, uint32_t price, uint64_t old_sh, uint32_t old_n, uint64_t new_sh,
               uint32_t new_n) {
        if constexpr (kTrackHash) {
            if (old_n) hash_ -= level_hash(locate, side, price, old_sh, old_n);
            if (new_n) hash_ += level_hash(locate, side, price, new_sh, new_n);
        }
    }

    void level_add(uint16_t locate, Side side, uint32_t price, uint32_t shares) {
        auto& v = levels(locate, side);
        const uint32_t key = key_of(price, side);
        bool found;
        const std::size_t i = search(v, key, found);
        if (found) {
            Level& l = v[i];
            touch(locate, side, price, l.shares, l.orders, l.shares + shares, l.orders + 1);
            l.shares += shares;
            l.orders += 1;
        } else {
            v.insert(v.begin() + static_cast<std::ptrdiff_t>(i), Level{key, 1, shares});
            touch(locate, side, price, 0, 0, shares, 1);
        }
    }

    void level_sub(const OrderEntry& o, uint64_t n, bool remove_order) {
        auto& v = levels(o.locate, o.side);
        bool found;
        const std::size_t i = search(v, key_of(o.price, o.side), found);
        Level& l = v[i];  // a live order always has its level
        const uint64_t new_sh = l.shares - n;
        const uint32_t new_n = l.orders - (remove_order ? 1 : 0);
        touch(o.locate, o.side, o.price, l.shares, l.orders, new_sh, new_n);
        if (new_n == 0) {
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            l.shares = new_sh;
            l.orders = new_n;
        }
    }

    void reduce(uint64_t ref, uint32_t n) {
        OrderEntry* e = orders_.find(ref);
        if (!e) [[unlikely]] {
            ++ctr_.unknown_ref;
            return;
        }
        if (n >= e->shares) {
            if (n > e->shares) ++ctr_.overfill;
            level_sub(*e, e->shares, true);
            orders_.erase(e);
            --ctr_.live_orders;
        } else {
            level_sub(*e, n, false);
            e->shares -= n;
        }
    }

    OrderMap orders_;
    std::vector<Stock> stocks_;
    std::vector<Symbol> symbols_;
    BookCounters ctr_;
    uint64_t hash_ = 0;
};

}  // namespace fh
