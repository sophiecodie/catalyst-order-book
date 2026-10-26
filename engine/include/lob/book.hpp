#pragma once

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lob/bitmap.hpp"
#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob {

struct BookConfig {
    Price base_price = 0;              // price of level 0
    std::uint32_t num_levels = 1 << 16;  // valid prices: [base_price, base_price + num_levels)
    std::uint32_t max_orders = 1 << 20;  // resting order capacity
};

// Single-instrument limit order book with price-time priority.
//
// Everything is sized in the constructor. After that, no operation allocates,
// throws, takes a lock or makes a virtual call. Errors come back as Result.
template <FillListener Listener>
class alignas(64) Book {
public:
    Book(const BookConfig& config, Listener& listener)
        : listener_(listener),
          base_(config.base_price),
          num_levels_(config.num_levels),
          bids_(new PriceLevel[config.num_levels]()),
          asks_(new PriceLevel[config.num_levels]()),
          bid_bits_(config.num_levels),
          ask_bits_(config.num_levels),
          pool_(config.max_orders) {
        // Twice the live maximum keeps the load factor low enough that abseil
        // reclaims tombstones in place instead of growing.
        ids_.reserve(std::size_t{config.max_orders} * 2);
    }

    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    // Matches against the opposite side while the limit crosses, then rests
    // the remainder. Rejected with PoolExhausted, before any matching, when
    // the pool is full.
    Result add_limit(OrderId id, Side side, Price price, Qty qty) noexcept {
        if (side != Side::Buy && side != Side::Sell) [[unlikely]] return Result::InvalidSide;
        if (qty == 0) [[unlikely]] return Result::InvalidQuantity;
        const std::size_t level = level_of(price);
        if (level >= num_levels_) [[unlikely]] return Result::InvalidPrice;
        if (ids_.contains(id)) [[unlikely]] return Result::DuplicateOrderId;
        if (pool_.full()) [[unlikely]] return Result::PoolExhausted;

        qty = side == Side::Buy ? match<Side::Buy>(id, level, qty) : match<Side::Sell>(id, level, qty);
        if (qty != 0) rest(id, side, level, qty);
        return Result::Ok;
    }

    // Immediate-or-cancel at any price: whatever cannot fill is dropped.
    Result add_market(OrderId id, Side side, Qty qty) noexcept {
        if (side != Side::Buy && side != Side::Sell) [[unlikely]] return Result::InvalidSide;
        if (qty == 0) [[unlikely]] return Result::InvalidQuantity;
        if (ids_.contains(id)) [[unlikely]] return Result::DuplicateOrderId;
        if (side == Side::Buy) match<Side::Buy>(id, num_levels_ - 1, qty);
        else match<Side::Sell>(id, 0, qty);
        return Result::Ok;
    }

    Result cancel(OrderId id) noexcept {
        const auto it = ids_.find(id);
        if (it == ids_.end()) [[unlikely]] return Result::UnknownOrderId;
        Order* order = it->second;
        ids_.erase(it);
        unlink(*order);
        pool_.release(pool_.handle_of(order));
        return Result::Ok;
    }

    // Reduces remaining quantity in place, keeping queue priority. The new
    // quantity must be positive and strictly smaller than the current one.
    Result modify(OrderId id, Qty new_qty) noexcept {
        const auto it = ids_.find(id);
        if (it == ids_.end()) [[unlikely]] return Result::UnknownOrderId;
        Order& order = *it->second;
        if (new_qty == 0 || new_qty >= order.qty) [[unlikely]] return Result::InvalidQuantity;
        side_levels(order.side)[order.level].total -= order.qty - new_qty;
        order.qty = new_qty;
        return Result::Ok;
    }

    TopOfBook best_bid() const noexcept { return top(bids_.get(), best_bid_); }
    TopOfBook best_ask() const noexcept { return top(asks_.get(), best_ask_); }

    std::uint64_t depth(Side side, Price price) const noexcept {
        const std::size_t level = level_of(price);
        if (level >= num_levels_) return 0;
        return (side == Side::Buy ? bids_ : asks_)[level].total;
    }

    std::size_t order_count() const noexcept { return pool_.in_use(); }
    std::uint32_t capacity() const noexcept { return pool_.capacity(); }
    bool contains(OrderId id) const noexcept { return ids_.contains(id); }

    // Empties the book without releasing or allocating memory.
    void clear() noexcept {
        for (auto it = ids_.begin(); it != ids_.end();) {
            Order* order = it->second;
            side_levels(order->side)[order->level] = PriceLevel{};
            pool_.release(pool_.handle_of(order));
            ids_.erase(it++);
        }
        bid_bits_.reset();
        ask_bits_.reset();
        best_bid_ = best_ask_ = LevelBitmap::npos;
        pool_.reset();
    }

private:
    using Handle = std::uint32_t;

    struct alignas(32) Order {
        OrderId id;
        Qty qty;
        std::uint32_t level;
        Handle prev;
        Handle next;
        Side side;
    };
    static_assert(sizeof(Order) == 32);

    struct PriceLevel {
        Handle head;
        Handle tail;
        std::uint64_t total;
    };
    static_assert(sizeof(PriceLevel) == 16);

    static constexpr Handle kNull = ObjectPool<Order>::kNull;

    std::size_t level_of(Price price) const noexcept {
        // Unsigned wrap sends prices below base_ out of range too.
        return static_cast<std::size_t>(static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(base_));
    }

    PriceLevel* side_levels(Side side) const noexcept { return side == Side::Buy ? bids_.get() : asks_.get(); }

    TopOfBook top(const PriceLevel* levels, std::size_t best) const noexcept {
        if (best == LevelBitmap::npos) return {0, 0};
        return {base_ + static_cast<Price>(best), levels[best].total};
    }

    // TakerSide is the incoming order's side. `limit` is the worst level it
    // may trade at. Returns the unfilled quantity.
    template <Side TakerSide>
    Qty match(OrderId taker, std::size_t limit, Qty qty) noexcept {
        constexpr bool kBuy = TakerSide == Side::Buy;
        PriceLevel* levels = kBuy ? asks_.get() : bids_.get();
        LevelBitmap& bits = kBuy ? ask_bits_ : bid_bits_;
        std::size_t& best = kBuy ? best_ask_ : best_bid_;

        while (qty != 0 && best != LevelBitmap::npos && (kBuy ? best <= limit : best >= limit)) {
            PriceLevel& level = levels[best];
            const Price price = base_ + static_cast<Price>(best);
            Handle h = level.head;
            while (h != kNull) {
                Order& maker = pool_[h];
                const Qty filled = std::min(maker.qty, qty);
                listener_.on_fill(Fill{maker.id, taker, price, filled});
                qty -= filled;
                maker.qty -= filled;
                level.total -= filled;
                if (maker.qty != 0) break;
                ids_.erase(maker.id);
                const Handle next = maker.next;
                pool_.release(h);
                h = next;
                if (qty == 0) break;
            }
            level.head = h;
            if (h != kNull) {
                pool_[h].prev = kNull;
                break;
            }
            level.tail = kNull;
            bits.clear(best);
            best = kBuy ? bits.next_up(best) : bits.next_down(best);
        }
        return qty;
    }

    void rest(OrderId id, Side side, std::size_t index, Qty qty) noexcept {
        const Handle h = pool_.acquire();
        PriceLevel& level = side_levels(side)[index];
        if (level.head == kNull) {
            if (side == Side::Buy) {
                bid_bits_.set(index);
                if (best_bid_ == LevelBitmap::npos || index > best_bid_) best_bid_ = index;
            } else {
                ask_bits_.set(index);
                if (index < best_ask_) best_ask_ = index;  // npos compares greater than everything
            }
        }
        Order& order = pool_[h];
        order = Order{id, qty, static_cast<std::uint32_t>(index), level.tail, kNull, side};
        if (level.tail != kNull) pool_[level.tail].next = h;
        else level.head = h;
        level.tail = h;
        level.total += qty;
        ids_.emplace(id, &order);
    }

    void unlink(Order& order) noexcept {
        PriceLevel& level = side_levels(order.side)[order.level];
        if (order.prev != kNull) pool_[order.prev].next = order.next;
        else level.head = order.next;
        if (order.next != kNull) pool_[order.next].prev = order.prev;
        else level.tail = order.prev;
        level.total -= order.qty;
        if (level.head != kNull) return;

        if (order.side == Side::Buy) {
            bid_bits_.clear(order.level);
            if (order.level == best_bid_) best_bid_ = bid_bits_.next_down(order.level);
        } else {
            ask_bits_.clear(order.level);
            if (order.level == best_ask_) best_ask_ = ask_bits_.next_up(order.level);
        }
    }

    // Hot state first: best indices and level/bitmap pointers share lines.
    Listener& listener_;
    Price base_;
    std::size_t num_levels_;
    std::size_t best_bid_ = LevelBitmap::npos;
    std::size_t best_ask_ = LevelBitmap::npos;
    std::unique_ptr<PriceLevel[]> bids_;
    std::unique_ptr<PriceLevel[]> asks_;
    LevelBitmap bid_bits_;
    LevelBitmap ask_bits_;
    ObjectPool<Order> pool_;
    absl::flat_hash_map<OrderId, Order*> ids_;
};

}  // namespace lob
