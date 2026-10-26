#include "lob/cycle_clock.hpp"

#include <chrono>
#include <thread>

namespace lob::cycle_clock {

namespace {

double measure() {
#if defined(__aarch64__)
    std::uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return 1e9 / static_cast<double>(freq);
#elif defined(__x86_64__) || defined(_M_X64)
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t1 = clock::now();
    const std::uint64_t c1 = now();
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ns / static_cast<double>(c1 - c0);
#else
    using period = std::chrono::steady_clock::period;
    return 1e9 * static_cast<double>(period::num) / static_cast<double>(period::den);
#endif
}

}  // namespace

double ns_per_tick() {
    static const double value = measure();
    return value;
}

}  // namespace lob::cycle_clock
