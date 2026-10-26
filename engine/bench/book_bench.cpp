#include <benchmark/benchmark.h>

#include <algorithm>
#include <vector>

#include "lob/book.hpp"
#include "synthetic.hpp"

namespace {

using namespace lob;

constexpr BookConfig kConfig{0, 1 << 16, 1 << 20};
constexpr Price kMid = 1 << 15;

struct SinkListener {
    std::uint64_t volume = 0;
    void on_fill(const Fill& f) noexcept { volume += f.qty; }
};

// Pre-drawn passive prices around the mid so the RNG stays out of the loop.
std::vector<Price> passive_prices(Side side, std::size_t n) {
    synthetic::Rng rng(42);
    std::vector<Price> prices(n);
    for (auto& p : prices) {
        const auto depth = static_cast<Price>(rng.below(8) * rng.below(8));
        p = side == Side::Buy ? kMid - 1 - depth : kMid + 1 + depth;
    }
    return prices;
}

// Resting adds into a book that already holds `range(0)` orders.
void BM_AddResting(benchmark::State& state) {
    SinkListener sink;
    Book<SinkListener> book(kConfig, sink);
    const auto preload = static_cast<std::size_t>(state.range(0));
    const auto prices = passive_prices(Side::Buy, 1 << 16);
    const std::size_t batch = kConfig.max_orders - preload;
    OrderId id = 0;
    const auto refill = [&] {
        book.clear();
        for (std::size_t i = 0; i < preload; ++i) book.add_limit(++id, Side::Buy, prices[i & 0xffff], 10);
    };
    refill();
    std::size_t added = 0;
    for (auto _ : state) {
        if (added == batch) [[unlikely]] {
            state.PauseTiming();
            refill();
            added = 0;
            state.ResumeTiming();
        }
        ++id;
        benchmark::DoNotOptimize(book.add_limit(id, Side::Buy, prices[id & 0xffff], 10));
        ++added;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddResting)->Arg(0)->Arg(10'000)->Arg(500'000);

// Cancels in random order from a book of `range(0)` resting orders.
void BM_Cancel(benchmark::State& state) {
    SinkListener sink;
    Book<SinkListener> book(kConfig, sink);
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto prices = passive_prices(Side::Sell, 1 << 16);
    std::vector<OrderId> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i + 1;
    synthetic::Rng rng(7);
    for (std::size_t i = n; i > 1; --i) std::swap(order[i - 1], order[rng.below(i)]);
    const auto refill = [&] {
        book.clear();
        for (std::size_t i = 0; i < n; ++i) book.add_limit(i + 1, Side::Sell, prices[i & 0xffff], 10);
    };
    refill();
    std::size_t next = 0;
    for (auto _ : state) {
        if (next == n) [[unlikely]] {
            state.PauseTiming();
            refill();
            next = 0;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(book.cancel(order[next++]));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cancel)->Arg(1'000)->Arg(100'000)->Arg(1'000'000);

// Aggressive limit orders that each consume `range(0)` whole resting orders,
// spread one per price level, from a deep ask side.
void BM_Match(benchmark::State& state) {
    SinkListener sink;
    Book<SinkListener> book(kConfig, sink);
    const auto per_order = static_cast<std::size_t>(state.range(0));
    constexpr std::size_t kDepth = 1 << 19;
    OrderId id = 0;
    std::size_t consumed = 0;
    const auto refill = [&] {
        book.clear();
        for (std::size_t i = 0; i < kDepth; ++i)
            book.add_limit(++id, Side::Sell, kMid + static_cast<Price>(i % 4096), 10);
        consumed = 0;
    };
    refill();
    for (auto _ : state) {
        if (consumed + per_order > kDepth) [[unlikely]] {
            state.PauseTiming();
            refill();
            state.ResumeTiming();
        }
        const TopOfBook ask = book.best_ask();
        benchmark::DoNotOptimize(
            book.add_limit(++id, Side::Buy, ask.price + static_cast<Price>(per_order), static_cast<Qty>(10 * per_order)));
        consumed += per_order;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["fills/op"] = static_cast<double>(per_order);
}
BENCHMARK(BM_Match)->Arg(1)->Arg(4)->Arg(32);

// Replays the synthetic mixed stream used by the latency harness.
void BM_ReplayMixed(benchmark::State& state) {
    synthetic::Config cfg;
    cfg.count = 1'000'000;
    const auto stream = synthetic::generate(cfg);
    SinkListener sink;
    Book<SinkListener> book(cfg.book, sink);
    for (auto _ : state) {
        for (const InboundMsg& msg : stream) benchmark::DoNotOptimize(apply(book, msg));
        state.PauseTiming();
        book.clear();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(stream.size()));
}
BENCHMARK(BM_ReplayMixed)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
