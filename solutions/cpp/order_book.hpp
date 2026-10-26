#pragma once

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Participant file. Keep the public API and result types unchanged.
struct Trade {
    std::int64_t maker_id, taker_id, price, quantity;
};
struct Level {
    std::int64_t price, quantity;
};

// Price-time priority book, same design as engine/ but standard library only.
//
// - Prices below kDense index flat per-side level arrays directly (price is the
//   index). The arrays are calloc'ed, so the OS hands out zero pages lazily and
//   only the levels actually touched cost anything.
// - A three-level bitmap per side finds the next non-empty level in a few
//   ctz/clz instructions.
// - Orders at a level form an intrusive doubly-linked FIFO of pool indices.
// - Order IDs map to nodes through a direct table (IDs are small and dense in
//   practice) with a hash-map fallback for anything else.
// - Prices at or above kDense (never seen in realistic traces) live in an
//   ordered overflow map. Every overflow price is above every dense price, so
//   overflow bids are always the best bids and overflow asks the worst asks.
class OrderBook {
    using i64 = std::int64_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    static constexpr i64 kDense = i64{1} << 20;
    static constexpr u64 kDirectIds = u64{1} << 24;
    static constexpr u32 kNil = 0;  // node 0 is a reserved sentinel

    struct Node {
        i64 id, price, qty;
        u32 prev, next;
        bool buy;
    };
    struct Queue {
        u32 head, tail;
        i64 total;
    };

    struct Free {
        void operator()(void* p) const { std::free(p); }
    };

    // Bit i of l0 is set when level i is non-empty; bit w of l1 when l0 word w
    // is non-zero; bit s of l2 when l1 word s is non-zero.
    class Bitmap {
        static constexpr u64 kWords0 = kDense >> 6, kWords1 = kDense >> 12, kWords2 = kDense >> 18;
        std::unique_ptr<u64[], Free> l0_{static_cast<u64*>(std::calloc(kWords0, sizeof(u64)))};
        u64 l1_[kWords1] = {};
        u64 l2_[kWords2] = {};

        static u64 bit(u64 i) { return u64{1} << (i & 63); }
        static u64 at_or_above(u64 i) { return ~u64{0} << (i & 63); }
        static u64 at_or_below(u64 i) { return ~u64{0} >> (63 - (i & 63)); }
        static u64 low(u64 m) { return static_cast<u64>(std::countr_zero(m)); }
        static u64 high(u64 m) { return 63 - static_cast<u64>(std::countl_zero(m)); }

    public:
        Bitmap() {
            if (!l0_) throw std::bad_alloc();
        }
        void set(u64 i) {
            l0_[i >> 6] |= bit(i);
            l1_[i >> 12] |= bit(i >> 6);
            l2_[i >> 18] |= bit(i >> 12);
        }
        void clear(u64 i) {
            const u64 w = i >> 6;
            if ((l0_[w] &= ~bit(i)) != 0) return;
            if ((l1_[w >> 6] &= ~bit(w)) != 0) return;
            l2_[w >> 12] &= ~bit(w >> 6);
        }
        // Lowest set index >= i, or -1.
        i64 next_up(u64 i) const {
            u64 w = i >> 6;
            if (u64 m = l0_[w] & at_or_above(i)) return static_cast<i64>((w << 6) | low(m));
            if (++w >= kWords0) return -1;
            u64 s = w >> 6;
            if (u64 m = l1_[s] & at_or_above(w)) return descend_up((s << 6) | low(m));
            if (++s >= kWords1) return -1;
            u64 t = s >> 6;
            u64 m = l2_[t] & at_or_above(s);
            while (!m) {
                if (++t >= kWords2) return -1;
                m = l2_[t];
            }
            return descend_up((((t << 6) | low(m)) << 6) | low(l1_[(t << 6) | low(m)]));
        }
        // Highest set index <= i, or -1.
        i64 next_down(u64 i) const {
            u64 w = i >> 6;
            if (u64 m = l0_[w] & at_or_below(i)) return static_cast<i64>((w << 6) | high(m));
            if (w-- == 0) return -1;
            u64 s = w >> 6;
            if (u64 m = l1_[s] & at_or_below(w)) return descend_down((s << 6) | high(m));
            if (s-- == 0) return -1;
            u64 t = s >> 6;
            u64 m = l2_[t] & at_or_below(s);
            while (!m) {
                if (t-- == 0) return -1;
                m = l2_[t];
            }
            return descend_down((((t << 6) | high(m)) << 6) | high(l1_[(t << 6) | high(m)]));
        }

    private:
        i64 descend_up(u64 w) const { return static_cast<i64>((w << 6) | low(l0_[w])); }
        i64 descend_down(u64 w) const { return static_cast<i64>((w << 6) | high(l0_[w])); }
    };

    std::unique_ptr<Queue[], Free> bidq_{static_cast<Queue*>(std::calloc(kDense, sizeof(Queue)))};
    std::unique_ptr<Queue[], Free> askq_{static_cast<Queue*>(std::calloc(kDense, sizeof(Queue)))};
    Bitmap bid_bits_, ask_bits_;
    i64 best_bid_ = -1, best_ask_ = -1;  // dense best levels, -1 when that side is empty
    std::map<i64, Queue> hi_bids_, hi_asks_;
    std::vector<Node> nodes_;
    u32 free_ = kNil;
    std::vector<u32> ids_;
    std::unordered_map<i64, u32> far_ids_;

    u32 find(i64 id) const {
        if (static_cast<u64>(id) < ids_.size()) [[likely]] return ids_[static_cast<u64>(id)];
        if (static_cast<u64>(id) < kDirectIds || far_ids_.empty()) return kNil;
        auto it = far_ids_.find(id);
        return it == far_ids_.end() ? kNil : it->second;
    }
    void map_id(i64 id, u32 h) {
        const u64 key = static_cast<u64>(id);
        if (key < kDirectIds) [[likely]] {
            if (key >= ids_.size()) [[unlikely]] {
                u64 size = ids_.size() * 2;
                while (size <= key) size *= 2;
                ids_.resize(size < kDirectIds ? size : kDirectIds, kNil);
            }
            ids_[key] = h;
        } else {
            far_ids_[id] = h;
        }
    }
    void unmap_id(i64 id) {
        if (static_cast<u64>(id) < ids_.size()) [[likely]] ids_[static_cast<u64>(id)] = kNil;
        else far_ids_.erase(id);
    }

    u32 alloc() {
        if (free_ != kNil) {
            const u32 h = free_;
            free_ = nodes_[h].next;
            return h;
        }
        nodes_.emplace_back();
        return static_cast<u32>(nodes_.size() - 1);
    }
    void release(u32 h) {
        nodes_[h].next = free_;
        free_ = h;
    }

    Queue& queue(bool buy, i64 price) {
        if (price < kDense) [[likely]] return (buy ? bidq_ : askq_)[price];
        return (buy ? hi_bids_ : hi_asks_)[price];
    }

    // Called once a level's queue has emptied.
    void drop_level(bool buy, i64 price) {
        if (price >= kDense) [[unlikely]] {
            (buy ? hi_bids_ : hi_asks_).erase(price);
            return;
        }
        if (buy) {
            bid_bits_.clear(static_cast<u64>(price));
            if (price == best_bid_) best_bid_ = bid_bits_.next_down(static_cast<u64>(price));
        } else {
            ask_bits_.clear(static_cast<u64>(price));
            if (price == best_ask_) best_ask_ = ask_bits_.next_up(static_cast<u64>(price));
        }
    }

    // Fills `qty` against the resting FIFO at `price`; returns what is left.
    i64 consume(Queue& q, i64 price, i64 taker, i64 qty, std::vector<Trade>& trades) {
        u32 h = q.head;
        while (h != kNil) {
            Node& n = nodes_[h];
            if (n.qty > qty) {
                n.qty -= qty;
                n.prev = kNil;
                q.total -= qty;
                q.head = h;
                trades.push_back({n.id, taker, price, qty});
                return 0;
            }
            trades.push_back({n.id, taker, price, n.qty});
            qty -= n.qty;
            q.total -= n.qty;
            unmap_id(n.id);
            const u32 next = n.next;
            release(h);
            h = next;
            if (qty == 0) break;
        }
        q.head = h;
        if (h != kNil) nodes_[h].prev = kNil;
        else q.tail = kNil;
        return qty;
    }

    std::vector<Trade> enter(i64 id, bool buy, i64 price, i64 qty) {
        std::vector<Trade> trades;
        if (buy) {
            for (;;) {
                Queue* q;
                i64 level;
                if (best_ask_ >= 0) [[likely]] {
                    level = best_ask_;
                    q = &askq_[level];
                } else if (!hi_asks_.empty()) [[unlikely]] {
                    auto it = hi_asks_.begin();
                    level = it->first;
                    q = &it->second;
                } else {
                    break;
                }
                if (level > price) break;
                qty = consume(*q, level, id, qty, trades);
                if (q->head == kNil) drop_level(false, level);
                if (qty == 0) return trades;
            }
        } else {
            for (;;) {
                Queue* q;
                i64 level;
                if (!hi_bids_.empty()) [[unlikely]] {
                    auto it = std::prev(hi_bids_.end());
                    level = it->first;
                    q = &it->second;
                } else if (best_bid_ >= 0) {
                    level = best_bid_;
                    q = &bidq_[level];
                } else {
                    break;
                }
                if (level < price) break;
                qty = consume(*q, level, id, qty, trades);
                if (q->head == kNil) drop_level(true, level);
                if (qty == 0) return trades;
            }
        }
        rest(id, buy, price, qty);
        return trades;
    }

    void rest(i64 id, bool buy, i64 price, i64 qty) {
        const u32 h = alloc();
        Queue& q = queue(buy, price);
        if (q.head == kNil && price < kDense) [[unlikely]] {
            if (buy) {
                bid_bits_.set(static_cast<u64>(price));
                if (price > best_bid_) best_bid_ = price;
            } else {
                ask_bits_.set(static_cast<u64>(price));
                if (best_ask_ < 0 || price < best_ask_) best_ask_ = price;
            }
        }
        nodes_[h] = {id, price, qty, q.tail, kNil, buy};
        if (q.tail != kNil) nodes_[q.tail].next = h;
        else q.head = h;
        q.tail = h;
        q.total += qty;
        map_id(id, h);
    }

    void erase_node(u32 h) {
        Node& n = nodes_[h];
        Queue& q = queue(n.buy, n.price);
        if (n.prev != kNil) nodes_[n.prev].next = n.next;
        else q.head = n.next;
        if (n.next != kNil) nodes_[n.next].prev = n.prev;
        else q.tail = n.prev;
        q.total -= n.qty;
        if (q.head == kNil) drop_level(n.buy, n.price);
        unmap_id(n.id);
        release(h);
    }

public:
    OrderBook() {
        if (!bidq_ || !askq_) throw std::bad_alloc();
        nodes_.reserve(1 << 12);
        nodes_.emplace_back();
        ids_.resize(1 << 12, kNil);
    }
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    std::vector<Trade> add(std::int64_t id, char side, std::int64_t price, std::int64_t quantity) {
        if ((side != 'B' && side != 'S') || price <= 0 || quantity <= 0 || find(id) != kNil) [[unlikely]]
            throw std::invalid_argument("invalid add");
        return enter(id, side == 'B', price, quantity);
    }
    std::vector<Trade> modify(std::int64_t id, std::int64_t price, std::int64_t quantity) {
        const u32 h = find(id);
        if (h == kNil || price <= 0 || quantity <= 0) [[unlikely]]
            throw std::invalid_argument("invalid modify");
        Node& n = nodes_[h];
        if (n.price == price) {
            queue(n.buy, price).total += quantity - n.qty;
            n.qty = quantity;
            return {};
        }
        const bool buy = n.buy;
        erase_node(h);
        return enter(id, buy, price, quantity);
    }
    void remove(std::int64_t id) {
        const u32 h = find(id);
        if (h == kNil) [[unlikely]] throw std::invalid_argument("unknown order");
        erase_node(h);
    }
    // C++ reserves "delete" as a keyword; remove is its equivalent.
    std::optional<Level> getBestBid() const {
        if (!hi_bids_.empty()) [[unlikely]] {
            const auto& [price, q] = *hi_bids_.rbegin();
            return Level{price, q.total};
        }
        if (best_bid_ < 0) return std::nullopt;
        return Level{best_bid_, bidq_[best_bid_].total};
    }
    std::optional<Level> getBestAsk() const {
        if (best_ask_ >= 0) [[likely]] return Level{best_ask_, askq_[best_ask_].total};
        if (hi_asks_.empty()) return std::nullopt;
        const auto& [price, q] = *hi_asks_.begin();
        return Level{price, q.total};
    }
};
