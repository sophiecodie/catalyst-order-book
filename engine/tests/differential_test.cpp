// Randomized differential test: lob::Book against the std::map reference.
// Every result code, every fill and both sides of the top of book must agree
// after every operation.

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "lob/book.hpp"
#include "reference_book.hpp"

namespace lob {
namespace {

struct Recorder {
    std::vector<Fill> fills;
    void on_fill(const Fill& f) noexcept { fills.push_back(f); }
};

struct Scenario {
    const char* name;
    BookConfig config;
    Price spread;          // how far around the middle prices are drawn
    std::uint32_t max_qty;
    int invalid_percent;   // share of deliberately bad requests
};

void run(const Scenario& s, std::uint64_t seed, int steps) {
    Recorder rec;
    Book<Recorder> book(s.config, rec);
    testing::ReferenceBook ref(s.config);
    std::mt19937_64 rng(seed);
    const Price mid = s.config.base_price + Price{s.config.num_levels} / 2;
    OrderId next_id = 1;

    for (int step = 0; step < steps; ++step) {
        const auto live = ref.live_ids();
        const bool invalid = static_cast<int>(rng() % 100) < s.invalid_percent;
        const auto pick_live = [&]() -> OrderId { return live.empty() ? 424242 : live[rng() % live.size()]; };
        const auto any_id = [&]() -> OrderId { return invalid && !live.empty() ? pick_live() : next_id++; };
        const Side side = rng() % 2 ? Side::Buy : Side::Sell;
        const auto qty = static_cast<Qty>(invalid && rng() % 4 == 0 ? 0 : 1 + rng() % s.max_qty);
        Price price = mid + static_cast<Price>(rng() % (2 * s.spread + 1)) - s.spread;
        if (invalid && rng() % 4 == 0) price = rng() % 2 ? s.config.base_price - 1 : mid + Price{s.config.num_levels};

        Result got, want;
        const unsigned kind = rng() % 100;
        if (kind < 50) {
            const OrderId id = any_id();
            got = book.add_limit(id, side, price, qty);
            want = ref.add_limit(id, side, price, qty);
        } else if (kind < 58) {
            const OrderId id = any_id();
            got = book.add_market(id, side, qty);
            want = ref.add_market(id, side, qty);
        } else if (kind < 85) {
            const OrderId id = invalid ? next_id + 1000 : pick_live();
            got = book.cancel(id);
            want = ref.cancel(id);
        } else {
            const OrderId id = invalid ? next_id + 1000 : pick_live();
            Qty new_qty = qty;
            if (!invalid && !live.empty()) {
                const Qty cur = ref.qty_of(id);
                new_qty = cur > 1 ? static_cast<Qty>(1 + rng() % (cur - 1)) : cur;  // cur==1: must reject
            }
            got = book.modify(id, new_qty);
            want = ref.modify(id, new_qty);
        }

        ASSERT_EQ(got, want) << s.name << " seed " << seed << " step " << step;
        ASSERT_EQ(rec.fills.size(), ref.fills.size()) << s.name << " seed " << seed << " step " << step;
        for (std::size_t i = 0; i < rec.fills.size(); ++i) {
            ASSERT_EQ(rec.fills[i].maker_id, ref.fills[i].maker_id) << "step " << step;
            ASSERT_EQ(rec.fills[i].taker_id, ref.fills[i].taker_id) << "step " << step;
            ASSERT_EQ(rec.fills[i].price, ref.fills[i].price) << "step " << step;
            ASSERT_EQ(rec.fills[i].qty, ref.fills[i].qty) << "step " << step;
        }
        rec.fills.clear();
        ref.fills.clear();

        const TopOfBook bb = book.best_bid(), rb = ref.best_bid();
        const TopOfBook ba = book.best_ask(), ra = ref.best_ask();
        ASSERT_EQ(bb.qty, rb.qty) << "step " << step;
        ASSERT_EQ(ba.qty, ra.qty) << "step " << step;
        if (rb.qty) ASSERT_EQ(bb.price, rb.price) << "step " << step;
        if (ra.qty) ASSERT_EQ(ba.price, ra.price) << "step " << step;
        ASSERT_EQ(book.order_count(), ref.order_count()) << "step " << step;
    }
}

const Scenario kScenarios[] = {
    // Narrow band: deep queues, lots of crossing and priority checks.
    {"narrow", {10'000, 64, 4096}, 6, 20, 5},
    // Wide sparse band: bitmap searches span many words and summary words.
    {"wide", {0, 1 << 16, 4096}, 30'000, 50, 5},
    // Tiny pool: exhaustion is hit constantly.
    {"tiny-pool", {500, 128, 24}, 20, 10, 5},
    // Book that spans exactly one bitmap word, with mostly invalid traffic.
    {"one-word", {0, 64, 256}, 40, 5, 40},
};

TEST(Differential, AgreesWithReferenceBook) {
    for (const Scenario& s : kScenarios)
        for (std::uint64_t seed = 1; seed <= 8; ++seed) run(s, seed, 20'000);
}

}  // namespace
}  // namespace lob
