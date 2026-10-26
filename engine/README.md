# Low-latency limit order book (C++20)

A single-instrument limit order book with price-time priority, built for predictable tail latency. It supports limit and market orders, cancels, and quantity reductions. Input and output go through single-producer single-consumer rings.

This folder is the full engine. The competition entry, [`../solutions/cpp/order_book.hpp`](../solutions/cpp/order_book.hpp), uses the same data-structure design but follows the contest's rules: one self-contained header, standard library only, exceptions on invalid calls, and a `modify` that can reprice an order or increase its quantity. The engine's test suite includes a differential test of that header, so `ctest` checks both.

## Layout

```
include/lob/   book.hpp (the book), bitmap.hpp, pool.hpp, types.hpp,
               messages.hpp (binary wire structs), engine.hpp (SPSC loop),
               cycle_clock.hpp
src/           cycle_clock.cpp (tick calibration), types.cpp
tests/         GoogleTest: unit, bitmap, differential, competition header
bench/         Google Benchmark: add, cancel, match, mixed replay
tools/         latency_harness.cpp, synthetic.hpp (order-flow generator)
```

## Build, test, run

You need CMake 3.24+ and a C++20 compiler (clang 15+ or GCC 12+). On macOS, `brew install cmake`. CMake's FetchContent downloads abseil, GoogleTest, Google Benchmark and rigtorp/SPSCQueue on the first configure.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build                  # or ./build/lob_tests
./build/lob_bench                       # Google Benchmark
./build/latency_harness                 # percentiles, inline + pipeline
```

Harness options:

| Option | Meaning |
| --- | --- |
| `--count N` | messages to replay (default 1,000,000) |
| `--seed S` | synthetic stream seed (default 1) |
| `--mode inline\|pipeline\|both` | which measurement to run (default both) |
| `--rate R` | pipeline send rate in messages/s (default 1,000,000) |
| `--engine-cpu C` | pin the book/engine thread (Linux only) |
| `--client-cpu C` | pin the producer thread (Linux only) |
| `--mlock` | `mlockall(MCL_CURRENT \| MCL_FUTURE)` before generating the stream |

Build options: `-DLOB_NATIVE=OFF` drops `-march=native`, `-DLOB_LTO=OFF` drops `-flto`, and `-DLOB_PGO=GENERATE|USE` is described below.

Release builds use `-O3 -march=native -flto`. The core library, the benchmarks and the harness also build with `-fno-exceptions -fno-rtti`. The test binary does not, because the competition-header test checks for thrown exceptions.

## Design

### Price levels: flat arrays plus a bitmap

Each side has a preallocated array of `PriceLevel {head, tail, total}` (16 bytes), indexed by `price - base_price`. There is no tree or `std::map`. You set the price range at startup with `BookConfig{base_price, num_levels, max_orders}`. An out-of-range price is rejected with `InvalidPrice`.

Best bid and best ask are cached as level indices. When the best level empties, a two-level bitmap finds the next non-empty level:

- **Level 0:** one bit per price level.
- **Summary:** one bit per level-0 word.

Searching costs one `ctz`/`clz` on the current word. If that word is empty, it scans the summary words (`num_levels / 4096` of them, so 16 for the default 65,536 levels) and then does one more `ctz`/`clz`. Prices cluster around the touch, so the first word almost always hits.

Resting or cancelling at a non-best level never searches. The book only searches when the best level itself empties.

### Orders: an intrusive FIFO in a fixed pool

Each `Order` node holds the id, quantity, level index, prev/next links and side, in 32 bytes (`alignas(32)`, two per cache line). Nodes come from a fixed-capacity `ObjectPool`. Links are 32-bit pool handles rather than pointers, which keeps the node at 32 bytes. Handle 0 is a reserved null, so a zeroed level means an empty queue.

Each level keeps a running `total`, so best-price queries are O(1) and never walk the queue.

### ID lookup: abseil `flat_hash_map`

The map is `absl::flat_hash_map<OrderId, Order*>` (not `std::unordered_map`), reserved at **twice** `max_orders`. The extra headroom matters: at low load, abseil marks most erased slots empty rather than leaving tombstones, and if it does need to clean up tombstones it does so in place. So the map never allocates after construction.

Cancel and modify are one hash probe plus O(1) unlinking.

### Matching semantics

- **Limit orders** match while their price crosses the opposite best. Fills always execute at the **resting maker's price**, in FIFO order within a level. Any unfilled remainder rests at the back of its level.
- **Market orders** are immediate-or-cancel: they sweep until filled or the opposite side is empty, and never rest.
- **Modify** reduces the remaining quantity in place and keeps queue priority. The new quantity must be positive and strictly smaller. To reprice, cancel and re-add. (The competition header supports reprice and increase, because that contest requires it.)
- **Pool exhaustion:** when the pool is full, a new limit order is rejected with `PoolExhausted` *before* any matching. That keeps the behaviour deterministic and side-effect free, at the cost of rejecting an order that would have filled completely. Market orders never need a node, so they still work when the pool is full.
- **Errors** come back as a `Result` enum and never throw. Rare paths are marked `[[unlikely]]`.

### Hot-path rules

- **No allocation after construction.** Level arrays, bitmaps, the pool and the hash map are all sized and touched in the constructors. `Book::clear()` empties the book without freeing or allocating, so benchmarks and the harness can replay.
- **No exceptions, RTTI, virtual calls or `std::function`.** The book is a template over its fill listener (`Book<Listener>`, checked by the `FillListener` concept), so `on_fill` inlines. The engine's listener writes straight into the outbound ring.
- **No locks.** The book is single-threaded. `Engine` drains a `rigtorp::SPSCQueue<InboundMsg>` and publishes fills, then one ack per message, on a `rigtorp::SPSCQueue<OutboundMsg>`. The queue's head and tail indices sit on separate cache lines. `Book` is `alignas(64)`, and its hot fields (best indices, base price, level and bitmap pointers) are declared first so they share cache lines.
- **Struct-of-arrays was considered and not used.** A level is 16 bytes and all of its fields are read together. The order node is 32 bytes and matching reads id, quantity and link together. Splitting either one would add cache misses rather than remove them.

### Wire format

`InboundMsg` (32 bytes) and `OutboundMsg` (40 bytes) are fixed-size, trivially copyable, standard-layout structs, ITCH-style: a one-byte type, then little-endian fields at fixed offsets pinned by `static_assert`. A feed parser can later decode straight into them. `apply(book, msg)` dispatches one message.

## Benchmarks and latency

### Methodology

- **Google Benchmark (`lob_bench`).**
  - `BM_AddResting/N`: passive adds into a book already holding N orders.
  - `BM_Cancel/N`: random-order cancels from a book of N orders.
  - `BM_Match/K`: an aggressive order that fully consumes K resting orders spread across levels.
  - `BM_ReplayMixed`: the whole synthetic stream.

  Refills run under `PauseTiming`.
- **Synthetic stream ([`tools/synthetic.hpp`](tools/synthetic.hpp)).** About 48% passive limit adds clustered near the touch, 35% cancels, 7% reductions, 5% market orders, and 5% aggressive limits that often sweep levels. The mid price random-walks and the book hovers around 20,000 resting orders. The generator runs a real `Book` while generating, so every cancel and modify targets a live order.
- **Inline mode.** One untimed warm-up pass, then one pass timed as a whole (the honest mean), then one pass with each book call bracketed by two cycle-counter reads. The book is `clear()`ed between passes, so every pass sees identical state.
- **Pipeline mode.** A producer thread stamps each message with the counter and pushes it at `--rate`. The engine thread processes it. The main thread receives the ack and records `now - stamp`, which covers both ring hops. The first 10% of messages are a warm-up and aren't reported.
- **Timers.**
  - x86-64: `rdtsc` with `lfence` on both sides, calibrated once against `steady_clock`.
  - AArch64: `CNTVCT_EL0` behind an `isb`, converted using `CNTFRQ_EL0`.

  The harness prints the counter's **measured** resolution. On Apple Silicon that is about 42 ns, which is coarser than one book operation, so inline percentiles are quantised to that step. The means stay accurate. For sub-10 ns per-op percentiles, use an x86 Linux box with an invariant TSC.

### Results on the development machine

These are illustrative only, not tuned numbers. The machine was an Apple M4 Pro (10 performance + 4 efficiency cores) on macOS with Apple clang 21. There was no pinning (macOS doesn't allow it), other work was running (load average around 7), and frequency scaling was on.

`lob_bench`:

| Benchmark | Time/op |
| --- | --- |
| AddResting (empty / 10k / 500k resting) | 13.9 / 13.5 / 13.2 ns |
| Cancel (1k / 100k / 1M resting) | 10.0 / 15.3 / 50.4 ns |
| Match, 1 / 4 / 32 fills per order | 38.6 / 108 / 715 ns |
| ReplayMixed (1M messages) | 14.0 ms (≈14 ns/msg) |

Cancel slows down as the book grows because, at 1M orders, the pool and hash map no longer fit in cache and each random cancel misses.

`latency_harness`, inline (1M messages, 14.7 ns/op mean without per-op timing; quantised to 42 ns):

| op | p50 | p99 | p99.9 |
| --- | --- | --- | --- |
| all | 42 | 84 | 167 |

The max values (7–29 µs) are OS preemption on an unpinned, loaded machine.

Pipeline at 1M msg/s: p50 292 ns, p99 3.7 µs, p99.9 13.8 µs. That is dominated by cross-core wakeups and scheduling on unpinned macOS threads. On an isolated Linux core it should be much tighter.

### Reproducing on Linux with thread pinning

Low, stable tails need a quiet core. A reasonable setup on a dedicated box:

1. **Isolate cores.** Add kernel boot parameters such as `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`. This keeps the scheduler, timer ticks and RCU callbacks off cores 2 and 3. Keep IRQs off them too: stop `irqbalance`, or set `/proc/irq/*/smp_affinity` to exclude them. Use physical cores, and turn off the SMT sibling of each one or leave it idle.
2. **Fix the frequency.** Run `sudo cpupower frequency-set -g performance`. Turn off turbo so runs are repeatable: `echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo`, or `echo 0 > /sys/devices/system/cpu/cpufreq/boost` on AMD. Turn off deep C-states in the BIOS, or with `intel_idle.max_cstate=1 processor.max_cstate=1`.
3. **Pin the threads.**
   ```sh
   taskset -c 2 ./build/latency_harness --mode inline --mlock
   ./build/latency_harness --mode pipeline --engine-cpu 2 --client-cpu 3 --mlock
   taskset -c 2 ./build/lob_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
   ```
   In pipeline mode, pin the engine and the client to two different isolated cores on the same socket, or the same CCX on AMD.
4. **Lock memory.** `--mlock` calls `mlockall` so nothing gets paged out. Raise the limit first with `ulimit -l unlimited`, or grant `CAP_IPC_LOCK`. The book already prefaults its arrays: constructors value-initialise them, and the harness runs a warm-up pass.
5. **Use huge pages.** With 2 MB pages, the level arrays, the pool and the hash map need far fewer TLB entries. Set `echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled` (or `madvise`), or reserve hugetlbfs pages with `vm.nr_hugepages`. The book allocates with plain `new`, so THP set to `always` is the zero-code option. A production build would allocate from `mmap(MAP_HUGETLB)` or `madvise(MADV_HUGEPAGE)` arenas.
6. **Check the TSC.** Confirm `constant_tsc nonstop_tsc` in `/proc/cpuinfo`. The harness calibrates against `steady_clock` once at start-up.

### PGO

```sh
cmake -S . -B build-pgo-gen -DLOB_PGO=GENERATE
cmake --build build-pgo-gen -j --target pgo-train   # runs harness + benches, merges into pgo-data/
cmake -S . -B build-pgo-use -DLOB_PGO=USE
cmake --build build-pgo-use -j
```

With clang this uses `-fprofile-instr-generate` and `-fprofile-instr-use`, and merges with `llvm-profdata` (found through `xcrun` on macOS). With GCC it uses `-fprofile-generate` and `-fprofile-use` (plus `-fprofile-partial-training`). On the development machine PGO built, trained and passed all tests, but its effect was within run-to-run noise (about ±15% on a loaded laptop). Measure it on a quiet, pinned machine before relying on it.

## Known limits and tail risks

- **Fixed price range.** A production book would re-centre its window as the market moves. The competition header handles any price instead, by sending prices of 2²⁰ and above to an ordered overflow map.
- **Tombstone cleanup.** Abseil's in-place cleanup is O(capacity) when it happens. The 2× reserve makes it rare, but it is the one hash-map behaviour that can spike latency.
- **Sparse sides.** A bitmap search that falls through to the summary is O(`num_levels` / 4096) words. Keep `num_levels` matched to the instrument's realistic range.
- **Out of scope for now:** kernel bypass, FPGAs, exchange protocols and self-trade prevention.
