#pragma once

// Synthetic order flow for benchmarks and the latency harness. Generation is
// not on any hot path, so it uses ordinary std containers freely.

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "lob/book.hpp"
#include "lob/messages.hpp"

namespace lob::synthetic {

struct Config {
    std::uint64_t seed = 1;
    std::size_t count = 1'000'000;
    BookConfig book{0, 1 << 16, 1 << 18};
    std::size_t target_live = 20'000;  // resting orders the flow hovers around
};

// SplitMix64: tiny, fast, and identical on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }

private:
    std::uint64_t state_;
};

namespace detail {

struct Tracker {
    struct Live {
        std::size_t pos;
        Qty qty;
    };
    std::vector<OrderId> ids;
    std::unordered_map<OrderId, Live> live;
    Qty taker_filled = 0;  // filled quantity of the message being applied

    void insert(OrderId id, Qty qty) {
        live.emplace(id, Live{ids.size(), qty});
        ids.push_back(id);
    }
    void erase(OrderId id) {
        auto it = live.find(id);
        if (it == live.end()) return;
        const std::size_t pos = it->second.pos;
        ids[pos] = ids.back();
        live[ids[pos]].pos = pos;
        ids.pop_back();
        live.erase(id);
    }
    void on_fill(const Fill& f) {
        taker_filled += f.qty;
        auto it = live.find(f.maker_id);
        if (it == live.end()) return;
        if ((it->second.qty -= f.qty) == 0) erase(f.maker_id);
    }
};

struct Listener {
    Tracker* tracker;
    void on_fill(const Fill& f) noexcept { tracker->on_fill(f); }
};

}  // namespace detail

// Roughly: 48% passive limit adds clustered near the touch, 35% cancels,
// 7% quantity reductions, 5% market orders and 5% aggressive limits that
// cross the spread and often sweep a few levels. The mid price random-walks.
inline std::vector<InboundMsg> generate(const Config& config) {
    Rng rng(config.seed);
    detail::Tracker tracker;
    detail::Listener listener{&tracker};
    Book<detail::Listener> book(config.book, listener);

    const Price lo = config.book.base_price;
    const Price hi = lo + static_cast<Price>(config.book.num_levels) - 1;
    Price mid = lo + static_cast<Price>(config.book.num_levels / 2);
    const auto clamp = [&](Price p) { return std::clamp(p, lo, hi); };
    const auto lots = [&]() -> Qty {
        return static_cast<Qty>(rng.below(20) == 0 ? 100 + rng.below(900) : 1 + rng.below(100));
    };

    std::vector<InboundMsg> out;
    out.reserve(config.count);
    OrderId next_id = 1;
    for (std::size_t i = 0; i < config.count; ++i) {
        if (i % 512 == 0) mid = std::clamp<Price>(mid + static_cast<Price>(rng.below(3)) - 1, lo + 1024, hi - 1024);

        std::uint64_t roll = rng.below(1000);
        const std::size_t live = tracker.ids.size();
        if (live == 0 || (live < config.target_live / 2 && roll >= 480)) roll = rng.below(480);
        else if (live >= config.target_live && roll < 480) roll = 480 + rng.below(350);

        InboundMsg msg;
        if (roll < 480) {
            const Side side = rng.below(2) ? Side::Buy : Side::Sell;
            const auto depth = static_cast<Price>(rng.below(8) * rng.below(8));
            const Price price = side == Side::Buy ? mid - 1 - depth : mid + 1 + depth;
            msg = make_add_limit(next_id++, side, clamp(price), lots());
        } else if (roll < 830) {
            msg = make_cancel(tracker.ids[rng.below(live)]);
        } else if (roll < 900) {
            const OrderId id = tracker.ids[rng.below(live)];
            const Qty qty = tracker.live[id].qty;
            msg = qty > 1 ? make_modify(id, static_cast<Qty>(1 + rng.below(qty - 1))) : make_cancel(id);
        } else if (roll < 950) {
            msg = make_add_market(next_id++, rng.below(2) ? Side::Buy : Side::Sell, lots());
        } else {
            const Side side = rng.below(2) ? Side::Buy : Side::Sell;
            const TopOfBook opposite = side == Side::Buy ? book.best_ask() : book.best_bid();
            const Price touch = opposite.qty ? opposite.price : mid;
            const auto through = static_cast<Price>(rng.below(4));
            const Price price = side == Side::Buy ? touch + through : touch - through;
            msg = make_add_limit(next_id++, side, clamp(price), lots());
        }

        tracker.taker_filled = 0;
        const Result result = apply(book, msg);
        if (result == Result::Ok) {
            if (msg.type == MsgType::AddLimit && book.contains(msg.id))
                tracker.insert(msg.id, msg.qty - tracker.taker_filled);
            else if (msg.type == MsgType::Cancel) tracker.erase(msg.id);
            else if (msg.type == MsgType::Modify) tracker.live[msg.id].qty = msg.qty;
        }
        out.push_back(msg);
    }
    return out;
}

}  // namespace lob::synthetic
