// Golden model for the order-level book: the simplest correct structure,
// kept forever, the way Version 1's GoldenBook is.
//
//   per stock and side: std::map<price, Level>, Level = std::list of orders
//   (front = first in queue);  orders: std::unordered_map<ref, location>
//
// Same ITCH semantics and counters as FifoBook. Slow and allocating on
// purpose: it is the reference FifoBook is checked against.
#pragma once

#include <array>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fh/book/book_common.hpp"
#include "fh/book/fifo_common.hpp"

namespace fh {

template <bool kTrackHash = true>
class GoldenFifoBook {
public:
    GoldenFifoBook() : books_(kMaxLocates), symbols_(kMaxLocates) {}

    void directory(uint16_t locate, const char (&sym)[8]) {
        ++ctr_.directory;
        symbols_[locate] = trim_fixed_symbol(sym);
    }

    void add(uint64_t ref, uint16_t locate, Side side, uint32_t price, uint32_t shares) {
        ++ctr_.adds;
        auto it = orders_.find(ref);
        if (it != orders_.end()) {  // duplicate: replace the old order
            ++ctr_.duplicate_ref;
            unlink(it->second);
            orders_.erase(it);
        } else if (++ctr_.live_orders > ctr_.peak_live_orders) {
            ctr_.peak_live_orders = ctr_.live_orders;
        }
        Level& lv = levels(locate, side)[price];
        const Summary old = summary(lv);
        lv.q.push_back(Order{ref, shares});
        lv.shares += shares;
        touch(locate, side, price, old, lv);
        orders_[ref] = Where{locate, side, price, std::prev(lv.q.end())};
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
        unlink(it->second);
        orders_.erase(it);
        --ctr_.live_orders;
    }
    void replace(uint64_t old_ref, uint64_t new_ref, uint32_t price, uint32_t shares) {
        ++ctr_.replaces;
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) {
            ++ctr_.unknown_ref;
            return;
        }
        const Where w = it->second;
        unlink(w);
        orders_.erase(it);
        --ctr_.live_orders;
        --ctr_.adds;
        add(new_ref, w.locate, w.side, price, shares);
    }

    uint64_t hash() const { return hash_; }
    const BookCounters& counters() const { return ctr_; }
    std::size_t order_count() const { return orders_.size(); }
    std::size_t depth(uint16_t locate, Side side) const { return levels(locate, side).size(); }
    const std::string& symbol(uint16_t locate) const { return symbols_[locate]; }

    std::optional<QueuePosition> queue_position(uint64_t ref) const {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return std::nullopt;
        const Where& w = it->second;
        QueuePosition qp;
        for (const Order& o : levels(w.locate, w.side).at(w.price).q) {
            if (o.ref == ref) break;
            ++qp.index;
            qp.shares_ahead += o.shares;
        }
        return qp;
    }

    template <class F>
    void top_into(uint16_t locate, Side side, std::size_t n, F&& f) const {
        const auto& m = levels(locate, side);
        auto emit = [&](auto first, auto last) {
            for (; first != last && n > 0; ++first, --n)
                f(LevelView{first->first, static_cast<uint32_t>(first->second.q.size()), first->second.shares});
        };
        if (side == Side::Buy) emit(m.rbegin(), m.rend());
        else emit(m.begin(), m.end());
    }
    std::vector<LevelView> top(uint16_t locate, Side side, std::size_t n) const {
        std::vector<LevelView> out;
        top_into(locate, side, n, [&](const LevelView& v) { out.push_back(v); });
        return out;
    }

    template <class F>
    void for_each_order(uint16_t locate, Side side, bool ascending, F&& f) const {
        const auto& m = levels(locate, side);
        auto walk = [&](auto first, auto last) {
            for (; first != last; ++first)
                for (const Order& o : first->second.q) f(first->first, o.ref, o.shares);
        };
        if (ascending) walk(m.begin(), m.end());
        else walk(m.rbegin(), m.rend());
    }

    uint64_t order_hash() const {
        uint64_t h = kOrderHashSeed;
        for (uint32_t i = 0; i < kMaxLocates; ++i) {
            const auto loc = static_cast<uint16_t>(i);
            for (Side s : {Side::Buy, Side::Sell}) {
                for_each_order(loc, s, true, [&](uint32_t price, uint64_t ref, uint32_t shares) {
                    h = order_hash_step(h, loc, s, price, ref, shares);
                });
            }
        }
        return h;
    }

    bool check_invariants(std::string* err = nullptr) const {
        std::size_t n = 0;
        for (const auto& sides : books_)
            for (const auto& m : sides)
                for (const auto& [price, lv] : m) {
                    uint64_t sh = 0;
                    for (const Order& o : lv.q) sh += o.shares;
                    if (lv.q.empty() || sh != lv.shares) {
                        if (err) *err = "level totals differ from its orders at price " + std::to_string(price);
                        return false;
                    }
                    n += lv.q.size();
                }
        if (n != orders_.size() || n != ctr_.live_orders) {
            if (err) *err = "live order count mismatch";
            return false;
        }
        return true;
    }

private:
    struct Order {
        uint64_t ref;
        uint32_t shares;
    };
    struct Level {
        std::list<Order> q;
        uint64_t shares = 0;
    };
    struct Summary {
        uint64_t shares;
        uint32_t count;
    };
    struct Where {
        uint16_t locate;
        Side side;
        uint32_t price;
        std::list<Order>::iterator it;
    };

    std::map<uint32_t, Level>& levels(uint16_t loc, Side s) { return books_[loc][s == Side::Buy ? 0 : 1]; }
    const std::map<uint32_t, Level>& levels(uint16_t loc, Side s) const { return books_[loc][s == Side::Buy ? 0 : 1]; }

    static Summary summary(const Level& l) { return {l.shares, static_cast<uint32_t>(l.q.size())}; }
    void touch(uint16_t loc, Side s, uint32_t price, Summary old, const Level& now) {
        if constexpr (kTrackHash) {
            if (old.count) hash_ -= level_hash(loc, s, price, old.shares, old.count);
            if (!now.q.empty()) hash_ += level_hash(loc, s, price, now.shares, static_cast<uint32_t>(now.q.size()));
        }
    }

    void unlink(const Where& w) {
        auto& m = levels(w.locate, w.side);
        auto lit = m.find(w.price);
        const Summary old = summary(lit->second);
        lit->second.shares -= w.it->shares;
        lit->second.q.erase(w.it);
        touch(w.locate, w.side, w.price, old, lit->second);
        if (lit->second.q.empty()) m.erase(lit);
    }

    void reduce(uint64_t ref, uint32_t n) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++ctr_.unknown_ref;
            return;
        }
        Where& w = it->second;
        if (n >= w.it->shares) {
            if (n > w.it->shares) ++ctr_.overfill;
            unlink(w);
            orders_.erase(it);
            --ctr_.live_orders;
            return;
        }
        Level& lv = levels(w.locate, w.side).at(w.price);
        const Summary old = summary(lv);
        w.it->shares -= n;  // keeps its place
        lv.shares -= n;
        touch(w.locate, w.side, w.price, old, lv);
    }

    std::vector<std::array<std::map<uint32_t, Level>, 2>> books_;
    std::unordered_map<uint64_t, Where> orders_;
    std::vector<std::string> symbols_;
    BookCounters ctr_;
    uint64_t hash_ = 0;
};

}  // namespace fh
