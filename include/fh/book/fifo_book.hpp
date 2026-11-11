// Order-level FIFO book (Version 2, Stage C0).
//
// Every price level keeps its orders in arrival order, so the queue position
// of any order is known. Nothing allocates after construction:
//
//   orders   fixed Pool<OrderNode>, found through a FixedHashMap (ref -> node)
//   levels   fixed Pool<LevelNode>; each level holds an intrusive doubly
//            linked list of its orders (head = first in queue)
//   near     per stock and side, a window of kWindow price slots, one per
//            cent, with a bitset for best-price search: O(1) level lookup
//            where ~99% of activity happens (measured on 2019-12-30)
//   far      levels outside the window, and sub-cent prices, live in a
//            pool-backed treap per stock and side: O(log n), any price
//
// Invariant: a level whose price is a whole cent inside the window range is
// in the window, never in the treap. When a new best price arrives outside
// the window, the window recentres on it and levels migrate both ways.
//
// ITCH semantics (same counters as Version 1's books):
//   A/F append at the tail;  E/C reduce, remove at zero;
//   X reduce, the order KEEPS its place;  D unlink;
//   U delete + add at the tail: the order LOSES its place.
//
// Exhausting any pool is a counted capacity error (FifoCounters), never a
// resize: growth would allocate on the hot path.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "fh/book/book_common.hpp"
#include "fh/book/fifo_common.hpp"
#include "fh/book/fixed_hash_map.hpp"
#include "fh/book/pool.hpp"

namespace fh {

struct FifoBookConfig {
    uint32_t max_orders = 4u << 20;             // live orders peaked at 1.92 M
    uint32_t max_levels = 2u << 20;             // live levels peaked at 784 K
    std::size_t order_map_slots = std::size_t{1} << 23;  // 75% limit = 6.3 M
    uint32_t max_windows = 24576;               // stock-sides with a window (17,812 on 2019-12-30)
};

template <bool kTrackHash = true>
class FifoBook {
public:
    static constexpr uint32_t kWindow = 512;  // slots: +/- 256 cents around the touch
    static constexpr uint32_t kTick = 100;    // one slot per $0.01 (price has 4 decimals)
    static constexpr uint32_t kSpan = kWindow * kTick;

    explicit FifoBook(const FifoBookConfig& c = {})
        : orders_(c.max_orders), levels_(c.max_levels), windows_(c.max_windows), map_(c.order_map_slots),
          sides_(kMaxLocates * 2), symbols_(kMaxLocates) {}

    // ---- message handlers ------------------------------------------------

    void directory(uint16_t locate, const char (&sym)[8]) {
        ++ctr_.directory;
        std::memcpy(symbols_[locate].data(), sym, 8);
    }

    void add(uint64_t ref, uint16_t locate, Side side, uint32_t price, uint32_t shares) {
        ++ctr_.adds;
#if defined(FH_INJECT_HOT_ALLOC)
        inject_log_.push_back(ref);  // deliberate: grows with no reserve, so it reallocates mid-replay
#endif
        bool inserted;
        uint32_t* slot = map_.insert(ref, inserted);
        if (!slot) {
            ++fifo_.map_full;
            return;
        }
        uint32_t o;
        if (!inserted) {  // duplicate reference: replace the old order in place
            ++ctr_.duplicate_ref;
            o = *slot;
            detach(o);
        } else {
            o = orders_.acquire();
            if (!o) {
                ++fifo_.order_pool_full;
                map_.erase(ref);
                return;
            }
            *slot = o;
            if (++ctr_.live_orders > ctr_.peak_live_orders) ctr_.peak_live_orders = ctr_.live_orders;
        }
        SideBook& sb = side_book(locate, side);
        const uint32_t lv = find_or_create_level(sb, side, price);
        if (!lv) {  // level pool exhausted: the order cannot be placed
            ++fifo_.level_pool_full;
            orders_.release(o);
            map_.erase(ref);
            --ctr_.live_orders;
            return;
        }
        OrderNode& n = orders_[o];
        n.ref = ref;
        n.price = price;
        n.shares = shares;
        n.locate = locate;
        n.side = side;
        append(lv, o);
    }

    void execute(uint64_t ref, uint32_t shares) {
        ++ctr_.executes;
        reduce(ref, shares);
    }

    void cancel(uint64_t ref, uint32_t shares) {  // keeps queue position
        ++ctr_.cancels;
        reduce(ref, shares);
    }

    void remove(uint64_t ref) {
        ++ctr_.deletes;
        uint32_t* slot = map_.find(ref);
        if (!slot) {
            ++ctr_.unknown_ref;
            return;
        }
        drop(*slot, ref);
    }

    // Loses queue position: the new order joins the tail, even at the same price.
    void replace(uint64_t old_ref, uint64_t new_ref, uint32_t price, uint32_t shares) {
        ++ctr_.replaces;
        uint32_t* slot = map_.find(old_ref);
        if (!slot) {
            ++ctr_.unknown_ref;
            return;
        }
        const OrderNode o = orders_[*slot];
        drop(*slot, old_ref);
        --ctr_.adds;  // the add below is part of the replace
        add(new_ref, o.locate, o.side, price, shares);
    }

    // ---- queries ----------------------------------------------------------

    uint64_t hash() const { return hash_; }  // Version 1 level hash
    const BookCounters& counters() const { return ctr_; }
    const FifoCounters& fifo_counters() const { return fifo_; }
    std::size_t order_count() const { return map_.size(); }
    std::size_t depth(uint16_t locate, Side side) const { return side_book(locate, side).levels; }
    std::string symbol(uint16_t locate) const { return trim_fixed_symbol(symbols_[locate].data()); }

    struct Usage {
        uint32_t orders_peak, orders_cap, levels_peak, levels_cap, windows_peak, windows_cap;
    };
    Usage usage() const {
        return {orders_.peak(), orders_.capacity(), levels_.peak(), levels_.capacity(), windows_.peak(),
                windows_.capacity()};
    }

    // Position of an order in its level's queue: 0 = first in line.
    std::optional<QueuePosition> queue_position(uint64_t ref) {
        uint32_t* slot = map_.find(ref);
        if (!slot) return std::nullopt;
        const uint32_t target = *slot;
        QueuePosition qp{0, 0};
        for (uint32_t o = levels_[orders_[target].level].head; o != target; o = orders_[o].next) {
            ++qp.index;
            qp.shares_ahead += orders_[o].shares;
        }
        return qp;
    }

    // Best n levels, best first.
    template <class F>
    void top_into(uint16_t locate, Side side, std::size_t n, F&& f) const {
        if (n == 0) return;
        for_each_level(locate, side, side == Side::Sell, [&](uint32_t h) {
            const LevelNode& l = levels_[h];
            f(LevelView{l.price, l.count, l.shares});
            return --n > 0;
        });
    }

    std::vector<LevelView> top(uint16_t locate, Side side, std::size_t n) const {
        std::vector<LevelView> out;
        top_into(locate, side, n, [&](const LevelView& v) { out.push_back(v); });
        return out;
    }

    // Visit every order of one level-ordered side, in price order (ascending
    // when `ascending`), then queue order: f(price, ref, shares).
    template <class F>
    void for_each_order(uint16_t locate, Side side, bool ascending, F&& f) const {
        for_each_level(locate, side, ascending, [&](uint32_t h) {
            for (uint32_t o = levels_[h].head; o; o = orders_[o].next) f(levels_[h].price, orders_[o].ref, orders_[o].shares);
            return true;
        });
    }

    // Order-level hash: every stock, both sides, levels ascending, orders in
    // queue order. Equal to GoldenFifoBook::order_hash() iff queues match.
    uint64_t order_hash() const {
        uint64_t h = kOrderHashSeed;
        for (uint32_t i = 0; i < kMaxLocates; ++i) {
            const auto loc = static_cast<uint16_t>(i);
            for (Side s : {Side::Buy, Side::Sell}) {
                if (!side_book(loc, s).levels) continue;
                for_each_order(loc, s, true, [&](uint32_t price, uint64_t ref, uint32_t shares) {
                    h = order_hash_step(h, loc, s, price, ref, shares);
                });
            }
        }
        return h;
    }

    bool check_invariants(std::string* err = nullptr) const {
        auto fail = [&](std::string m) {
            if (err) *err = std::move(m);
            return false;
        };
        std::size_t orders_seen = 0;
        for (uint32_t i = 0; i < kMaxLocates; ++i) {
            const auto loc = static_cast<uint16_t>(i);
            for (Side s : {Side::Buy, Side::Sell}) {
                const SideBook& sb = side_book(loc, s);
                uint32_t levels = 0, in_window = 0;
                int64_t last_price = -1;
                std::string problem;
                for_each_level(loc, s, true, [&](uint32_t h) {
                    const LevelNode& l = levels_[h];
                    ++levels;
                    if (static_cast<int64_t>(l.price) <= last_price) problem = "levels not strictly ascending";
                    last_price = l.price;
                    const bool fits = sb.window && l.price % kTick == 0 && l.price >= sb.base && l.price - sb.base < kSpan;
                    if (fits != l.in_window) problem = "window/treap placement violates the invariant";
                    if (l.in_window) {
                        ++in_window;
                        const uint32_t idx = (l.price - sb.base) / kTick;
                        if (windows_[sb.window].slot[idx] != h || !bit(windows_[sb.window], idx))
                            problem = "window slot or bitset does not point at its level";
                    }
                    uint64_t shares = 0;
                    uint32_t count = 0, prev = 0;
                    for (uint32_t o = l.head; o; prev = o, o = orders_[o].next) {
                        const OrderNode& n = orders_[o];
                        if (n.prev != prev) problem = "broken prev link";
                        if (n.level != h || n.price != l.price || n.locate != loc || n.side != s)
                            problem = "order does not belong to its level";
                        if (n.shares == 0) problem = "order with zero shares";
                        shares += n.shares;
                        ++count;
                    }
                    if (prev != l.tail) problem = "tail does not match the last order";
                    if (shares != l.shares || count != l.count) problem = "level totals differ from its orders";
                    orders_seen += count;
                    return problem.empty();
                });
                if (!problem.empty()) return fail(problem + " (locate " + std::to_string(i) + ")");
                if (levels != sb.levels || in_window != sb.window_levels)
                    return fail("level counters differ (locate " + std::to_string(i) + ")");
            }
        }
        if (orders_seen != map_.size() || orders_seen != ctr_.live_orders)
            return fail("live orders: " + std::to_string(orders_seen) + " in levels, " + std::to_string(map_.size()) +
                        " in map, " + std::to_string(ctr_.live_orders) + " counted");
        return true;
    }

private:
    struct OrderNode {
        uint64_t ref;
        uint32_t price, shares;
        uint32_t level, prev, next;
        uint16_t locate;
        Side side;
    };
    struct LevelNode {
        uint32_t price, count;
        uint64_t shares;
        uint32_t head, tail;     // FIFO of orders
        uint32_t left, right;    // treap links (far levels only)
        uint32_t prio;           // treap heap priority
        bool in_window;
    };
    struct Window {
        uint32_t slot[kWindow];  // level handle per cent, 0 = empty
        uint64_t bits[kWindow / 64];
    };
    struct SideBook {
        uint32_t window = 0;   // handle into windows_, 0 = none yet
        uint32_t base = 0;     // price of slot 0
        uint32_t root = 0;     // treap of far levels
        uint32_t levels = 0;   // all levels on this side
        uint32_t window_levels = 0;
    };

    SideBook& side_book(uint16_t loc, Side s) { return sides_[loc * 2u + (s == Side::Buy ? 0u : 1u)]; }
    const SideBook& side_book(uint16_t loc, Side s) const { return sides_[loc * 2u + (s == Side::Buy ? 0u : 1u)]; }

    static bool bit(const Window& w, uint32_t i) { return (w.bits[i >> 6] >> (i & 63)) & 1; }
    static void set_bit(Window& w, uint32_t i) { w.bits[i >> 6] |= uint64_t{1} << (i & 63); }
    static void clear_bit(Window& w, uint32_t i) { w.bits[i >> 6] &= ~(uint64_t{1} << (i & 63)); }

    // First set bit at index >= i, or kWindow.
    static uint32_t next_bit(const Window& w, uint32_t i) {
        while (i < kWindow) {
            const uint64_t word = w.bits[i >> 6] >> (i & 63);
            if (word) return i + static_cast<uint32_t>(__builtin_ctzll(word));
            i = (i | 63) + 1;
        }
        return kWindow;
    }
    // Last set bit at index <= i (i < kWindow), or UINT32_MAX.
    static uint32_t prev_bit(const Window& w, uint32_t i) {
        for (;;) {
            const uint32_t shift = 63 - (i & 63);
            const uint64_t word = w.bits[i >> 6] << shift;
            if (word) return i - static_cast<uint32_t>(__builtin_clzll(word));
            if (i < 64) return UINT32_MAX;
            i = (i & ~63u) - 1;
        }
    }

    bool in_window_range(const SideBook& sb, uint32_t price) const {
        return sb.window && price % kTick == 0 && price >= sb.base && price - sb.base < kSpan;
    }
    static uint32_t centred_base(uint32_t price) {
        const uint32_t half = kSpan / 2;
        return price >= half ? price - half : 0;
    }

    // ---- treap over levels_ (key = price) ----------------------------------

    uint32_t merge(uint32_t a, uint32_t b) {
        if (!a) return b;
        if (!b) return a;
        if (levels_[a].prio > levels_[b].prio) {
            levels_[a].right = merge(levels_[a].right, b);
            return a;
        }
        levels_[b].left = merge(a, levels_[b].left);
        return b;
    }
    // l gets keys < key, r gets keys >= key.
    void split(uint32_t t, uint32_t key, uint32_t& l, uint32_t& r) {
        if (!t) {
            l = r = 0;
            return;
        }
        if (levels_[t].price < key) {
            split(levels_[t].right, key, levels_[t].right, r);
            l = t;
        } else {
            split(levels_[t].left, key, l, levels_[t].left);
            r = t;
        }
    }
    void treap_insert(uint32_t& root, uint32_t h) {
        LevelNode& n = levels_[h];
        n.left = n.right = 0;
        n.prio = static_cast<uint32_t>(rng_next());
        uint32_t l, r;
        split(root, n.price, l, r);
        root = merge(merge(l, h), r);
    }
    void treap_erase(uint32_t& root, uint32_t price) {
        uint32_t l, m, r;
        split(root, price, l, m);
        split(m, price + 1, m, r);
        root = merge(l, r);
    }
    uint32_t treap_find(uint32_t t, uint32_t price) const {
        while (t && levels_[t].price != price) t = price < levels_[t].price ? levels_[t].left : levels_[t].right;
        return t;
    }
    uint64_t rng_next() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return rng_;
    }

    // Visit levels in price order, merging the window and the treap; f(h)
    // returns false to stop. No allocation: recursion over the treap.
    template <class F>
    void for_each_level(uint16_t loc, Side side, bool ascending, F&& f) const {
        const SideBook& sb = side_book(loc, side);
        const Window* w = sb.window ? &windows_[sb.window] : nullptr;
        uint32_t cur = UINT32_MAX;
        if (w) cur = ascending ? next_bit(*w, 0) : prev_bit(*w, kWindow - 1);
        auto valid = [&] { return w && cur != UINT32_MAX && cur < kWindow; };
        auto wprice = [&] { return sb.base + cur * kTick; };
        auto advance = [&] { cur = ascending ? next_bit(*w, cur + 1) : (cur == 0 ? UINT32_MAX : prev_bit(*w, cur - 1)); };
        bool go = true;
        auto visit = [&](auto&& self, uint32_t t) -> void {
            if (!t || !go) return;
            const LevelNode& n = levels_[t];
            self(self, ascending ? n.left : n.right);
            if (!go) return;
            while (go && valid() && (ascending ? wprice() < n.price : wprice() > n.price)) {
                go = f(w->slot[cur]);
                advance();
            }
            if (!go) return;
            go = f(t);
            self(self, ascending ? n.right : n.left);
        };
        visit(visit, sb.root);
        while (go && valid()) {
            go = f(w->slot[cur]);
            advance();
        }
    }

    // Best price on a side, if any level exists.
    bool best_price(const SideBook& sb, Side side, uint32_t& best) const {
        bool have = false;
        if (sb.window && sb.window_levels) {
            const Window& w = windows_[sb.window];
            const uint32_t i = side == Side::Buy ? prev_bit(w, kWindow - 1) : next_bit(w, 0);
            best = sb.base + i * kTick;
            have = true;
        }
        if (sb.root) {
            uint32_t t = sb.root;
            if (side == Side::Buy)
                while (levels_[t].right) t = levels_[t].right;
            else
                while (levels_[t].left) t = levels_[t].left;
            const uint32_t p = levels_[t].price;
            if (!have || (side == Side::Buy ? p > best : p < best)) best = p;
            have = true;
        }
        return have;
    }

    // Move the window to start at new_base: window levels that fall outside
    // go to the treap, treap levels (whole cents) that fall inside come in.
    void recentre(SideBook& sb, uint32_t new_base) {
        ++fifo_.recentres;
        Window& w = windows_[sb.window];
        uint32_t keep[kWindow];
        std::memset(keep, 0, sizeof keep);
        for (uint32_t i = next_bit(w, 0); i < kWindow; i = next_bit(w, i + 1)) {
            const uint32_t h = w.slot[i];
            const uint32_t p = sb.base + i * kTick;
            if (p >= new_base && p - new_base < kSpan) {
                keep[(p - new_base) / kTick] = h;
            } else {
                levels_[h].in_window = false;
                treap_insert(sb.root, h);
                --sb.window_levels;
            }
        }
        std::memset(w.bits, 0, sizeof w.bits);
        for (uint32_t i = 0; i < kWindow; ++i) {
            w.slot[i] = keep[i];
            if (keep[i]) set_bit(w, i);
        }
        sb.base = new_base;
        // Pull treap levels inside [new_base, new_base + span).
        uint32_t l, m, r;
        split(sb.root, new_base, l, m);
        split(m, new_base + kSpan, m, r);
        sb.root = merge(l, r);
        absorb(sb, m);
    }
    void absorb(SideBook& sb, uint32_t t) {  // re-place every node of subtree t
        if (!t) return;
        const uint32_t left = levels_[t].left, right = levels_[t].right;
        absorb(sb, left);
        absorb(sb, right);
        LevelNode& n = levels_[t];
        if (n.price % kTick == 0) {
            Window& w = windows_[sb.window];
            const uint32_t idx = (n.price - sb.base) / kTick;
            w.slot[idx] = t;
            set_bit(w, idx);
            n.in_window = true;
            n.left = n.right = 0;
            ++sb.window_levels;
        } else {
            treap_insert(sb.root, t);  // sub-cent price inside the range stays in the treap
        }
    }

    uint32_t find_or_create_level(SideBook& sb, Side side, uint32_t price) {
        if (in_window_range(sb, price)) {
            Window& w = windows_[sb.window];
            const uint32_t idx = (price - sb.base) / kTick;
            if (w.slot[idx]) return w.slot[idx];
            return place_in_window(sb, price);
        }
        if (const uint32_t t = treap_find(sb.root, price)) return t;
        // A new level outside the window. Recentre if it is a whole cent and
        // either there is no window yet, the window is empty, or the price is
        // a new best (the market moved past the window).
        if (price % kTick == 0) {
            if (!sb.window) {
                sb.window = windows_.acquire();
                if (!sb.window) ++fifo_.window_pool_full;  // fall through to the treap
                else {
                    sb.base = centred_base(price);
                    recentre(sb, sb.base);
                    return place_in_window(sb, price);
                }
            } else {
                uint32_t best;
                const bool have = best_price(sb, side, best);
                if (sb.window_levels == 0 || !have || (side == Side::Buy ? price > best : price < best)) {
                    recentre(sb, centred_base(price));
                    return place_in_window(sb, price);
                }
            }
        }
        const uint32_t h = levels_.acquire();
        if (!h) return 0;
        ++fifo_.far_levels;
        LevelNode& n = levels_[h];
        n.price = price;
        treap_insert(sb.root, h);
        ++sb.levels;
        return h;
    }
    uint32_t place_in_window(SideBook& sb, uint32_t price) {
        const uint32_t h = levels_.acquire();
        if (!h) return 0;
        LevelNode& n = levels_[h];
        n.price = price;
        n.in_window = true;
        Window& w = windows_[sb.window];
        const uint32_t idx = (price - sb.base) / kTick;
        w.slot[idx] = h;
        set_bit(w, idx);
        ++sb.window_levels;
        ++sb.levels;
        return h;
    }
    void remove_level(SideBook& sb, uint32_t h) {
        LevelNode& n = levels_[h];
        if (n.in_window) {
            Window& w = windows_[sb.window];
            const uint32_t idx = (n.price - sb.base) / kTick;
            w.slot[idx] = 0;
            clear_bit(w, idx);
            --sb.window_levels;
        } else {
            treap_erase(sb.root, n.price);
        }
        --sb.levels;
        levels_.release(h);
    }

    void touch(const OrderNode& o, const LevelNode& l, uint64_t old_sh, uint32_t old_n) {
        if constexpr (kTrackHash) {
            if (old_n) hash_ -= level_hash(o.locate, o.side, l.price, old_sh, old_n);
            if (l.count) hash_ += level_hash(o.locate, o.side, l.price, l.shares, l.count);
        }
    }

    void append(uint32_t lv, uint32_t o) {
        LevelNode& l = levels_[lv];
        OrderNode& n = orders_[o];
        const uint64_t old_sh = l.shares;
        const uint32_t old_n = l.count;
        n.level = lv;
        n.prev = l.tail;
        n.next = 0;
        if (l.tail) orders_[l.tail].next = o;
        else l.head = o;
        l.tail = o;
        l.shares += n.shares;
        ++l.count;
        touch(n, l, old_sh, old_n);
    }

    // Unlink an order from its level (removing the level if it empties).
    void detach(uint32_t o) {
        OrderNode& n = orders_[o];
        LevelNode& l = levels_[n.level];
        const uint64_t old_sh = l.shares;
        const uint32_t old_n = l.count;
        if (n.prev) orders_[n.prev].next = n.next;
        else l.head = n.next;
        if (n.next) orders_[n.next].prev = n.prev;
        else l.tail = n.prev;
        l.shares -= n.shares;
        --l.count;
        touch(n, l, old_sh, old_n);
        if (l.count == 0) remove_level(side_book(n.locate, n.side), n.level);
    }

    void drop(uint32_t o, uint64_t ref) {
        detach(o);
        orders_.release(o);
        map_.erase(ref);
        --ctr_.live_orders;
    }

    void reduce(uint64_t ref, uint32_t n) {
        uint32_t* slot = map_.find(ref);
        if (!slot) {
            ++ctr_.unknown_ref;
            return;
        }
        const uint32_t o = *slot;
        OrderNode& node = orders_[o];
        if (n >= node.shares) {
            if (n > node.shares) ++ctr_.overfill;
            drop(o, ref);
            return;
        }
        LevelNode& l = levels_[node.level];  // partial: keeps its place in the queue
        const uint64_t old_sh = l.shares;
        node.shares -= n;
        l.shares -= n;
        touch(node, l, old_sh, l.count);
    }

    Pool<OrderNode> orders_;
    Pool<LevelNode> levels_;
    Pool<Window> windows_;
    FixedHashMap map_;
    std::vector<SideBook> sides_;
    std::vector<std::array<char, 8>> symbols_;
    BookCounters ctr_;
    FifoCounters fifo_;
    uint64_t hash_ = 0;
    uint64_t rng_ = 0x2545F4914F6CDD1Dull;
#if defined(FH_INJECT_HOT_ALLOC)
    std::vector<uint64_t> inject_log_;  // perfguard's must-fail CI check (CMake FH_INJECT_HOT_ALLOC)
#endif
};

}  // namespace fh
