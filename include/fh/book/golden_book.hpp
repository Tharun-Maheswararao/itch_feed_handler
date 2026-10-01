// Golden model order book: the simplest correct implementation, kept forever.
// std::unordered_map for order lookup, std::map per side for price levels.
// Every optimization is checked against this.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "fh/book/book_common.hpp"

namespace fh {

template <bool kTrackHash = true>
class GoldenBook {
public:
    GoldenBook() : stocks_(kMaxLocates), symbols_(kMaxLocates) {}

    void directory(uint16_t locate, const char (&sym)[8]) {
        ++ctr_.directory;
        symbols_[locate] = trim_symbol(sym);
    }

    void add(uint64_t ref, uint16_t locate, Side side, uint32_t price, uint32_t shares) {
        ++ctr_.adds;
        auto [it, inserted] = orders_.try_emplace(ref, Order{price, shares, locate, side});
        if (!inserted) {  // never expected in a valid feed; replace the old one
            ++ctr_.duplicate_ref;
            erase_order(it->second);
            it->second = Order{price, shares, locate, side};
        } else {
            ++ctr_.live_orders;
            if (ctr_.live_orders > ctr_.peak_live_orders) ctr_.peak_live_orders = ctr_.live_orders;
        }
        Level& lvl = side_map(locate, side)[price];
        const Level old = lvl;
        lvl.shares += shares;
        lvl.orders += 1;
        touch(locate, side, price, old, lvl);
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
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++ctr_.unknown_ref;
            return;
        }
        erase_order(it->second);
        orders_.erase(it);
        --ctr_.live_orders;
    }

    // Side and stock are kept from the original order (spec 1.4.5).
    void replace(uint64_t old_ref, uint64_t new_ref, uint32_t price, uint32_t shares) {
        ++ctr_.replaces;
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) {
            ++ctr_.unknown_ref;
            return;
        }
        const Order o = it->second;
        erase_order(o);
        orders_.erase(it);
        --ctr_.live_orders;
        --ctr_.adds;  // the add below is part of the replace, not a new add
        add(new_ref, o.locate, o.side, price, shares);
    }

    uint64_t hash() const { return hash_; }
    const BookCounters& counters() const { return ctr_; }
    const std::string& symbol(uint16_t locate) const { return symbols_[locate]; }
    std::size_t order_count() const { return orders_.size(); }

    // Best n levels, best first.
    std::vector<LevelView> top(uint16_t locate, Side side, std::size_t n) const {
        std::vector<LevelView> out;
        top_into(locate, side, n, [&](const LevelView& lv) { out.push_back(lv); });
        return out;
    }

    template <class F>
    void top_into(uint16_t locate, Side side, std::size_t n, F&& f) const {
        const Stock& s = stocks_[locate];
        auto emit = [&](auto first, auto last) {
            for (; first != last && n > 0; ++first, --n) f(LevelView{first->first, first->second.orders, first->second.shares});
        };
        if (side == Side::Buy)
            emit(s.bids.rbegin(), s.bids.rend());
        else
            emit(s.asks.begin(), s.asks.end());
    }

    std::size_t depth(uint16_t locate, Side side) const {
        const Stock& s = stocks_[locate];
        return side == Side::Buy ? s.bids.size() : s.asks.size();
    }

    // Rebuild each level from the orders and compare with the stored levels.
    bool check_invariants(std::string* err = nullptr) const {
        std::vector<Stock> rebuilt(kMaxLocates);
        for (const auto& [ref, o] : orders_) {
            if (o.shares == 0) return fail(err, "order with zero shares: " + std::to_string(ref));
            auto& lvl = (o.side == Side::Buy ? rebuilt[o.locate].bids : rebuilt[o.locate].asks)[o.price];
            lvl.shares += o.shares;
            lvl.orders += 1;
        }
        for (std::size_t i = 0; i < kMaxLocates; ++i) {
            if (rebuilt[i].bids != stocks_[i].bids || rebuilt[i].asks != stocks_[i].asks)
                return fail(err, "level aggregates differ from orders for locate " + std::to_string(i));
        }
        if (orders_.size() != ctr_.live_orders) return fail(err, "live order counter mismatch");
        return true;
    }

private:
    struct Order {
        uint32_t price;
        uint32_t shares;
        uint16_t locate;
        Side side;
    };
    struct Level {
        uint64_t shares = 0;
        uint32_t orders = 0;
        bool operator==(const Level&) const = default;
    };
    struct Stock {
        std::map<uint32_t, Level> bids;  // best = highest = rbegin
        std::map<uint32_t, Level> asks;  // best = lowest  = begin
    };

    static bool fail(std::string* err, std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    }

    std::map<uint32_t, Level>& side_map(uint16_t locate, Side side) {
        return side == Side::Buy ? stocks_[locate].bids : stocks_[locate].asks;
    }

    void touch(uint16_t locate, Side side, uint32_t price, const Level& old, const Level& now) {
        if constexpr (kTrackHash) {
            if (old.orders) hash_ -= level_hash(locate, side, price, old.shares, old.orders);
            if (now.orders) hash_ += level_hash(locate, side, price, now.shares, now.orders);
        }
    }

    // Take `n` shares off the level and, if `remove_order`, one order.
    void level_sub(const Order& o, uint64_t n, bool remove_order) {
        auto& m = side_map(o.locate, o.side);
        auto it = m.find(o.price);
        const Level old = it->second;
        it->second.shares -= n;
        if (remove_order) it->second.orders -= 1;
        touch(o.locate, o.side, o.price, old, it->second);
        if (it->second.orders == 0) m.erase(it);
    }

    void erase_order(const Order& o) { level_sub(o, o.shares, true); }

    void reduce(uint64_t ref, uint32_t n) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++ctr_.unknown_ref;
            return;
        }
        Order& o = it->second;
        if (n >= o.shares) {
            if (n > o.shares) ++ctr_.overfill;
            erase_order(o);
            orders_.erase(it);
            --ctr_.live_orders;
        } else {
            level_sub(o, n, false);
            o.shares -= n;
        }
    }

    std::unordered_map<uint64_t, Order> orders_;
    std::vector<Stock> stocks_;
    std::vector<std::string> symbols_;
    BookCounters ctr_;
    uint64_t hash_ = 0;
};

}  // namespace fh
