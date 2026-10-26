#pragma once

// Deliberately simple std::map + std::deque book with the same contract as
// lob::Book, used as the oracle in differential tests.

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/book.hpp"

namespace lob::testing {

class ReferenceBook {
public:
    explicit ReferenceBook(const BookConfig& config) : config_(config) {}

    std::vector<Fill> fills;

    Result add_limit(OrderId id, Side side, Price price, Qty qty) {
        if (side != Side::Buy && side != Side::Sell) return Result::InvalidSide;
        if (qty == 0) return Result::InvalidQuantity;
        if (price < config_.base_price || price >= config_.base_price + Price{config_.num_levels})
            return Result::InvalidPrice;
        if (orders_.count(id)) return Result::DuplicateOrderId;
        if (orders_.size() >= config_.max_orders) return Result::PoolExhausted;
        qty = match(id, side, price, qty);
        if (qty) {
            (side == Side::Buy ? bids_ : asks_)[price].push_back(id);
            orders_[id] = {side, price, qty};
        }
        return Result::Ok;
    }

    Result add_market(OrderId id, Side side, Qty qty) {
        if (side != Side::Buy && side != Side::Sell) return Result::InvalidSide;
        if (qty == 0) return Result::InvalidQuantity;
        if (orders_.count(id)) return Result::DuplicateOrderId;
        match(id, side, side == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min(),
              qty);
        return Result::Ok;
    }

    Result cancel(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return Result::UnknownOrderId;
        auto& levels = it->second.side == Side::Buy ? bids_ : asks_;
        auto& queue = levels[it->second.price];
        queue.erase(std::find(queue.begin(), queue.end(), id));
        if (queue.empty()) levels.erase(it->second.price);
        orders_.erase(it);
        return Result::Ok;
    }

    Result modify(OrderId id, Qty qty) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return Result::UnknownOrderId;
        if (qty == 0 || qty >= it->second.qty) return Result::InvalidQuantity;
        it->second.qty = qty;
        return Result::Ok;
    }

    TopOfBook best_bid() const {
        if (bids_.empty()) return {0, 0};
        return {bids_.rbegin()->first, total(bids_.rbegin()->second)};
    }
    TopOfBook best_ask() const {
        if (asks_.empty()) return {0, 0};
        return {asks_.begin()->first, total(asks_.begin()->second)};
    }
    std::size_t order_count() const { return orders_.size(); }

    std::vector<OrderId> live_ids() const {
        std::vector<OrderId> ids;
        for (const auto& [id, order] : orders_) ids.push_back(id);
        return ids;
    }
    Qty qty_of(OrderId id) const { return orders_.at(id).qty; }

private:
    struct Order {
        Side side;
        Price price;
        Qty qty;
    };

    std::uint64_t total(const std::deque<OrderId>& queue) const {
        std::uint64_t sum = 0;
        for (OrderId id : queue) sum += orders_.at(id).qty;
        return sum;
    }

    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        while (qty) {
            const bool buy = side == Side::Buy;
            auto& levels = buy ? asks_ : bids_;
            if (levels.empty()) break;
            auto it = buy ? levels.begin() : std::prev(levels.end());
            if (buy ? it->first > limit : it->first < limit) break;
            auto& queue = it->second;
            Order& maker = orders_.at(queue.front());
            const Qty filled = std::min(qty, maker.qty);
            fills.push_back({queue.front(), taker, it->first, filled});
            qty -= filled;
            maker.qty -= filled;
            if (maker.qty == 0) {
                orders_.erase(queue.front());
                queue.pop_front();
                if (queue.empty()) levels.erase(it);
            }
        }
        return qty;
    }

    BookConfig config_;
    std::map<Price, std::deque<OrderId>> bids_, asks_;
    std::unordered_map<OrderId, Order> orders_;
};

}  // namespace lob::testing
