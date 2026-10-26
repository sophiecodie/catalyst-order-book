// Replays a synthetic order stream and reports per-operation latency
// percentiles from the cycle counter.
//
//   inline:   one thread; each book call is bracketed by two counter reads.
//   pipeline: client thread -> SPSC ring -> engine thread -> SPSC ring ->
//             client; measures ack latency including both queue hops.

#include <sys/mman.h>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "lob/cycle_clock.hpp"
#include "lob/engine.hpp"
#include "synthetic.hpp"

namespace {

using lob::cycle_clock::now;

struct Options {
    std::size_t count = 1'000'000;
    std::uint64_t seed = 1;
    std::string mode = "both";
    double rate = 1'000'000;  // pipeline messages per second
    int engine_cpu = -1;
    int client_cpu = -1;
    bool lock_memory = false;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: latency_harness [--count N] [--seed S] [--mode inline|pipeline|both]\n"
                 "                       [--rate MSGS_PER_SEC] [--engine-cpu C] [--client-cpu C] [--mlock]\n");
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (arg == "--count") o.count = std::strtoull(value(), nullptr, 10);
        else if (arg == "--seed") o.seed = std::strtoull(value(), nullptr, 10);
        else if (arg == "--mode") o.mode = value();
        else if (arg == "--rate") o.rate = std::strtod(value(), nullptr);
        else if (arg == "--engine-cpu") o.engine_cpu = std::atoi(value());
        else if (arg == "--client-cpu") o.client_cpu = std::atoi(value());
        else if (arg == "--mlock") o.lock_memory = true;
        else usage();
    }
    if (o.mode != "inline" && o.mode != "pipeline" && o.mode != "both") usage();
    return o;
}

void pin_current_thread(int cpu) {
    if (cpu < 0) return;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
#else
    std::fprintf(stderr, "warning: thread pinning is only implemented on Linux (cpu %d ignored)\n", cpu);
#endif
}

enum Category : std::uint8_t { kAddRest, kAddMatch, kMarket, kCancel, kModify, kCategories };
constexpr const char* kCategoryNames[] = {"add (rests)", "add (matches)", "market", "cancel", "modify"};

struct Sample {
    std::uint32_t ticks;
    std::uint8_t category;
};

void report(const char* title, std::vector<Sample>& samples, double ns_per_tick) {
    std::printf("\n%s\n", title);
    std::printf("  %-14s %10s %9s %9s %9s %9s %10s\n", "op", "count", "p50", "p99", "p99.9", "max", "mean");
    std::vector<std::uint32_t> ticks;
    ticks.reserve(samples.size());
    const auto row = [&](const char* name, int category) {
        ticks.clear();
        for (const Sample& s : samples)
            if (category < 0 || s.category == category) ticks.push_back(s.ticks);
        if (ticks.empty()) return;
        std::sort(ticks.begin(), ticks.end());
        const auto pct = [&](double p) {
            const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(ticks.size())));
            return static_cast<double>(ticks[std::max<std::size_t>(rank, 1) - 1]) * ns_per_tick;
        };
        double sum = 0;
        for (std::uint32_t t : ticks) sum += t;
        std::printf("  %-14s %10zu %9.0f %9.0f %9.0f %9.0f %10.1f\n", name, ticks.size(), pct(0.50), pct(0.99),
                    pct(0.999), static_cast<double>(ticks.back()) * ns_per_tick,
                    sum / static_cast<double>(ticks.size()) * ns_per_tick);
    };
    for (int c = 0; c < kCategories; ++c) row(kCategoryNames[c], c);
    row("all", -1);
    std::printf("  (nanoseconds)\n");
}

struct CountingListener {
    std::uint64_t fills = 0;
    std::uint64_t volume = 0;
    void on_fill(const lob::Fill& f) noexcept {
        ++fills;
        volume += f.qty;
    }
};

Category categorize(const lob::InboundMsg& msg, bool filled) {
    switch (msg.type) {
        case lob::MsgType::AddLimit: return filled ? kAddMatch : kAddRest;
        case lob::MsgType::AddMarket: return kMarket;
        case lob::MsgType::Cancel: return kCancel;
        case lob::MsgType::Modify: return kModify;
    }
    return kAddRest;
}

void run_inline(const Options& opt, const lob::synthetic::Config& cfg, const std::vector<lob::InboundMsg>& stream,
                double ns_per_tick) {
    pin_current_thread(opt.engine_cpu);
    CountingListener listener;
    lob::Book<CountingListener> book(cfg.book, listener);

    // Pass 1 warms caches, branch predictors and every page the book touches;
    // pass 2 measures throughput without per-op timing; pass 3 times each op.
    for (const auto& msg : stream) lob::apply(book, msg);
    book.clear();

    const std::uint64_t t0 = now();
    for (const auto& msg : stream) lob::apply(book, msg);
    const std::uint64_t t1 = now();
    book.clear();

    std::vector<Sample> samples(stream.size());
    std::size_t rejected = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        const std::uint64_t fills_before = listener.fills;
        const std::uint64_t start = now();
        const lob::Result r = lob::apply(book, stream[i]);
        const std::uint64_t end = now();
        rejected += r != lob::Result::Ok;
        samples[i] = {static_cast<std::uint32_t>(end - start), categorize(stream[i], listener.fills != fills_before)};
    }

    std::printf("\ninline: %.1f ns/op mean without per-op timing (%zu ops, %zu rejected, %" PRIu64 " fills)\n",
                static_cast<double>(t1 - t0) * ns_per_tick / static_cast<double>(stream.size()), stream.size(),
                rejected, listener.fills / 3);
    report("inline per-op latency (book call only)", samples, ns_per_tick);
}

void run_pipeline(const Options& opt, const lob::synthetic::Config& cfg, const std::vector<lob::InboundMsg>& stream,
                  double ns_per_tick) {
    lob::InboundQueue in(1 << 16);
    lob::OutboundQueue out(1 << 18);
    lob::Engine engine(cfg.book, in, out);
    std::atomic<bool> stop{false};

    std::thread engine_thread([&] {
        pin_current_thread(opt.engine_cpu);
        engine.run(stop);
    });

    // The producer paces sends on the cycle counter; this thread collects
    // acks. The first 10% of messages are a warm-up and are not reported.
    const auto interval = static_cast<std::uint64_t>(1e9 / opt.rate / ns_per_tick);
    std::thread producer([&] {
        pin_current_thread(opt.client_cpu);
        std::uint64_t deadline = now();
        for (lob::InboundMsg msg : stream) {
            while (now() < deadline) {
            }
            deadline += interval;
            msg.timestamp = now();
            in.push(msg);
        }
    });

    const std::size_t warmup = stream.size() / 10;
    std::vector<Sample> samples;
    samples.reserve(stream.size() - warmup);
    std::size_t acks = 0;
    bool filled = false;
    while (acks < stream.size()) {
        const lob::OutboundMsg* msg = out.front();
        if (msg == nullptr) continue;
        if (msg->type == lob::OutType::Fill) {
            filled = true;
        } else {
            const std::uint64_t t = now();
            if (acks >= warmup)
                samples.push_back({static_cast<std::uint32_t>(t - msg->timestamp), categorize(stream[acks], filled)});
            ++acks;
            filled = false;
        }
        out.pop();
    }
    producer.join();
    stop.store(true);
    engine_thread.join();

    char title[160];
    std::snprintf(title, sizeof title, "pipeline ack latency (send -> SPSC -> book -> SPSC -> receive) at %.0f msg/s",
                  opt.rate);
    report(title, samples, ns_per_tick);
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);
    if (opt.lock_memory && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) std::perror("warning: mlockall");

    lob::synthetic::Config cfg;
    cfg.seed = opt.seed;
    cfg.count = opt.count;
    const std::vector<lob::InboundMsg> stream = lob::synthetic::generate(cfg);

    const double ns_per_tick = lob::cycle_clock::ns_per_tick();
    // The advertised frequency can overstate resolution (Apple M4 reports a
    // 1 GHz counter that advances in steps of about 41.7 ns), so measure the
    // smallest observable step as well as the cost of a back-to-back read.
    std::vector<std::uint64_t> overhead(100'000), steps(overhead.size());
    for (std::size_t i = 0; i < overhead.size(); ++i) {
        const std::uint64_t a = now();
        std::uint64_t b = now();
        overhead[i] = b - a;
        while (b == a) b = now();
        const std::uint64_t c = b;
        while (b == c) b = now();
        steps[i] = b - c;  // one full counter step, measured edge to edge
    }
    std::sort(overhead.begin(), overhead.end());
    std::sort(steps.begin(), steps.end());
    const double resolution_ns = static_cast<double>(steps[steps.size() / 2]) * ns_per_tick;
    std::printf("timer: %.3f ns/tick, resolution %.1f ns, back-to-back read median %.1f ns\n", ns_per_tick,
                resolution_ns, static_cast<double>(overhead[overhead.size() / 2]) * ns_per_tick);
    if (resolution_ns > 5)
        std::printf("note: the counter is coarser than a single book operation, so per-op percentiles are\n"
                    "      quantised to multiples of %.1f ns. Means remain accurate over many samples.\n",
                    resolution_ns);

    std::size_t counts[4] = {};
    for (const auto& m : stream) {
        switch (m.type) {
            case lob::MsgType::AddLimit: ++counts[0]; break;
            case lob::MsgType::AddMarket: ++counts[1]; break;
            case lob::MsgType::Cancel: ++counts[2]; break;
            case lob::MsgType::Modify: ++counts[3]; break;
        }
    }
    std::printf("stream: %zu msgs (seed %" PRIu64 "): %zu limit, %zu market, %zu cancel, %zu modify\n",
                stream.size(), opt.seed, counts[0], counts[1], counts[2], counts[3]);

    if (opt.mode != "pipeline") run_inline(opt, cfg, stream, ns_per_tick);
    if (opt.mode != "inline") run_pipeline(opt, cfg, stream, ns_per_tick);
    return 0;
}
