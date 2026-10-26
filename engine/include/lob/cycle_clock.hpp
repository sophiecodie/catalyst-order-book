#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace lob::cycle_clock {

// Raw tick counter: TSC on x86-64, the generic timer (CNTVCT_EL0) on AArch64.
// The fences keep the read from drifting across the code being timed.
inline std::uint64_t now() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#elif defined(__aarch64__)
    std::uint64_t t;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(t)::"memory");
    return t;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Nanoseconds per tick. AArch64 reads the architected frequency register; x86
// calibrates the TSC against steady_clock once (about 50 ms, first call only).
double ns_per_tick();

}  // namespace lob::cycle_clock
