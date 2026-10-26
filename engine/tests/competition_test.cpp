// Differential test for the competition submission (solutions/cpp/order_book.hpp),
// which has its own contract: modify can reprice or grow an order, invalid
// calls throw std::invalid_argument, and best-level calls return optionals.

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <map>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "order_book.hpp"

namespace {

using i64 = std::int64_t;

class Reference {
public:
    std::vector<Trade> add(i64 id, char side, i64 price, i64 qty) {
        if (orders_.count(id) || (side != 'B' && side != 'S') || price <= 0 || qty <= 0)
            throw std::invalid_argument("add");
        return place(id, side, price, qty);
    }
    std::vector<Trade> modify(i64 id, i64 price, i64 qty) {
        auto it = orders_.find(id);
        if (it == orders_.end() || price <= 0 || qty <= 0) throw std::invalid_argument("modify");
        if (it->second.price == price) {
            it->second.qty = qty;
            return {};
        }
        const char side = it->second.side;
        remove(id);
        return place(id, side, price, qty);
    }
    void remove(i64 id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) throw std::invalid_argument("remove");
        auto& levels = it->second.side == 'B' ? bids_ : asks_;
        auto& q = levels[it->second.price];
        q.erase(std::find(q.begin(), q.end(), id));
        if (q.empty()) levels.erase(it->second.price);
        orders_.erase(it);
    }
    std::optional<Level> best_bid() const { return bids_.empty() ? std::nullopt : level(*bids_.rbegin()); }
    std::optional<Level> best_ask() const { return asks_.empty() ? std::nullopt : level(*asks_.begin()); }

    std::vector<i64> live() const {
        std::vector<i64> ids;
        for (const auto& [id, o] : orders_) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    i64 price_of(i64 id) const { return orders_.at(id).price; }

private:
    struct Order {
        char side;
        i64 price, qty;
    };
    using Levels = std::map<i64, std::deque<i64>>;

    std::optional<Level> level(const Levels::value_type& entry) const {
        i64 total = 0;
        for (i64 id : entry.second) total += orders_.at(id).qty;
        return Level{entry.first, total};
    }

    std::vector<Trade> place(i64 id, char side, i64 price, i64 qty) {
        std::vector<Trade> trades;
        auto& other = side == 'B' ? asks_ : bids_;
        while (qty && !other.empty()) {
            auto it = side == 'B' ? other.begin() : std::prev(other.end());
            if (side == 'B' ? it->first > price : it->first < price) break;
            const i64 maker = it->second.front();
            Order& m = orders_.at(maker);
            const i64 filled = std::min(qty, m.qty);
            trades.push_back({maker, id, it->first, filled});
            qty -= filled;
            if ((m.qty -= filled) == 0) {
                orders_.erase(maker);
                it->second.pop_front();
                if (it->second.empty()) other.erase(it);
            }
        }
        if (qty) {
            (side == 'B' ? bids_ : asks_)[price].push_back(id);
            orders_[id] = {side, price, qty};
        }
        return trades;
    }

    Levels bids_, asks_;
    std::unordered_map<i64, Order> orders_;
};

bool same(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].maker_id != b[i].maker_id || a[i].taker_id != b[i].taker_id || a[i].price != b[i].price ||
            a[i].quantity != b[i].quantity)
            return false;
    return true;
}

bool same(const std::optional<Level>& a, const std::optional<Level>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->price == b->price && a->quantity == b->quantity);
}

template <class F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

struct Shape {
    const char* name;
    i64 center, spread, reprice;
    std::vector<i64> far_prices;  // occasionally used, including overflow-map prices >= 2^20
    std::vector<i64> far_ids;     // occasionally used, outside the direct ID table
};

void run(const Shape& shape, std::uint64_t seed, int steps) {
    OrderBook book;
    Reference ref;
    std::mt19937_64 rng(seed);
    i64 next_id = 1;
    std::size_t next_far_id = 0;

    const auto draw_price = [&](i64 around, i64 spread) {
        if (!shape.far_prices.empty() && rng() % 50 == 0) return shape.far_prices[rng() % shape.far_prices.size()];
        return std::max<i64>(1, around + static_cast<i64>(rng() % (2 * spread + 1)) - spread);
    };

    for (int step = 0; step < steps; ++step) {
        const auto live = ref.live();
        const unsigned kind = rng() % 100;
        SCOPED_TRACE(::testing::Message() << shape.name << " seed " << seed << " step " << step);
        if (kind < 2) {
            // Invalid requests must throw from both.
            const i64 id = live.empty() || rng() % 2 ? -77 : live[rng() % live.size()];
            const unsigned what = rng() % 5;
            const auto bad = [&](auto& b) {
                switch (what) {
                    case 0: b.add(id, 'X', 100, 1); break;
                    case 1: b.add(next_id, 'B', 0, 1); break;
                    case 2: b.add(next_id, 'S', 100, -5); break;
                    case 3: b.modify(-77, 100, 1); break;
                    default: b.remove(-77); break;
                }
            };
            EXPECT_TRUE(throws([&] { bad(book); }));
            EXPECT_TRUE(throws([&] { bad(ref); }));
            if (!live.empty()) {
                const i64 existing = live[rng() % live.size()];
                EXPECT_TRUE(throws([&] { book.add(existing, 'B', 100, 1); }));
                EXPECT_TRUE(throws([&] { book.modify(existing, 100, 0); }));
            }
        } else if (live.empty() || kind < 46) {
            i64 id = next_id++;
            if (!shape.far_ids.empty() && rng() % 20 == 0) id = shape.far_ids[next_far_id++ % shape.far_ids.size()];
            if (std::find(live.begin(), live.end(), id) != live.end()) id = next_id++;
            const char side = rng() % 2 ? 'B' : 'S';
            const i64 price = draw_price(shape.center, shape.spread);
            const i64 qty = 1 + static_cast<i64>(rng() % 100);
            ASSERT_TRUE(same(book.add(id, side, price, qty), ref.add(id, side, price, qty)));
        } else if (kind < 70) {
            const i64 id = live[rng() % live.size()];
            const i64 old = ref.price_of(id);
            const i64 price = rng() % 2 ? old : draw_price(old, shape.reprice);
            const i64 qty = 1 + static_cast<i64>(rng() % 100);
            ASSERT_TRUE(same(book.modify(id, price, qty), ref.modify(id, price, qty)));
        } else if (kind < 82) {
            const i64 id = live[rng() % live.size()];
            book.remove(id);
            ref.remove(id);
        }
        ASSERT_TRUE(same(book.getBestBid(), ref.best_bid()));
        ASSERT_TRUE(same(book.getBestAsk(), ref.best_ask()));
    }
}

TEST(Competition, PublicFixture) {
    OrderBook b;
    EXPECT_FALSE(b.getBestBid());
    b.add(1, 'B', 100, 10);
    b.add(2, 'B', 100, 5);
    auto t = b.add(3, 'S', 99, 12);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].maker_id, 1);
    EXPECT_EQ(t[1].quantity, 2);
    EXPECT_EQ(b.getBestBid()->quantity, 3);
    b.modify(2, 100, 8);  // grows in place, keeps priority
    EXPECT_EQ(b.getBestBid()->quantity, 8);
    b.add(4, 'B', 100, 1);
    t = b.add(5, 'S', 100, 4);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].maker_id, 2);
}

TEST(Competition, MatchesReference) {
    const Shape shapes[] = {
        // The judge's workload shape: prices near 1000, reprices within +-20.
        {"judge-like", 1000, 45, 20, {}, {}},
        // Everything in one or two bitmap words; deep queues.
        {"narrow", 500, 3, 3, {}, {}},
        // Spread across many l1/l2 bitmap words, plus overflow-map prices.
        {"wide", 500'000, 400'000, 300'000, {(1 << 20) - 1, 1 << 20, (1 << 20) + 7, i64{1} << 40, 1}, {}},
        // IDs far outside the direct table, including negative ones.
        {"far-ids", 1000, 45, 20, {}, {i64{1} << 30, i64{1} << 50, -3, -(i64{1} << 40), (i64{1} << 24) - 1}},
    };
    for (const Shape& shape : shapes)
        for (std::uint64_t seed = 1; seed <= 6; ++seed) run(shape, seed, 30'000);
}

}  // namespace
