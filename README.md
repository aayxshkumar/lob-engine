# lob-engine

A limit order book and matching engine in C++20, built to be measured: every design change comes with a benchmark showing what it bought.

> **Status: v0 baseline.** Core book, tests and benchmark harness are done. The roadmap below lists what comes next.

## What it does

- **Price-time priority matching** for limit and market orders, on one instrument.
- **Add, cancel and modify.** Reducing quantity at the same price keeps the order's queue position. Changing the price or increasing quantity sends it to the back.
- **Integer tick prices**, never floating point.
- **No allocation per order:** orders live in a pre-allocated pool and are chained per price level in an intrusive doubly linked list, so a cancel is O(1) once the order is found.

```cpp
lob::OrderBook book;
std::vector<lob::Trade> trades;
book.add_limit(1, lob::Side::Sell, 10'050, 100, trades);  // rests
book.add_limit(2, lob::Side::Buy,  10'060,  40, trades);  // trades 40 @ 10'050
book.cancel(1);
```

## Build and run

Requirements: CMake 3.24+ and a C++20 compiler (tested with Apple clang 17; CI also runs GCC and Clang on Ubuntu 24.04). GoogleTest and Google Benchmark are downloaded by CMake.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # 22 tests, including the differential test below
./build/lob_bench                            # throughput (Google Benchmark)
./build/lob_latency 1000000                  # per-operation p50 / p99 / p99.9
```

Sanitizer build: `-DLOB_SANITIZERS=address,undefined` (Linux) or `-DLOB_SANITIZERS=undefined` (macOS, where ASan currently hangs at startup on macOS 26).

## Correctness

- **Differential test.** 20,000 random operations (adds, cancels, all three kinds of modify, market orders) run through both `OrderBook` and a deliberately naive [`ReferenceBook`](tests/reference_book.hpp), a flat vector scanned linearly. After every step, trades and book state must match exactly.
- **The tests are checked by mutation.** Five bugs were planted by hand: priority kept on a size increase, market orders obeying a limit, trades at the taker's price, crossing one tick early, and a stale level total after cancel. The differential test caught all five.
- **20 unit tests** cover price and time priority, partial fills, multi-level sweeps, cancel from the middle of a queue, modify semantics, duplicate and zero-quantity rejection, and pool reuse.
- **Randomised invariant test:** the book is never crossed after any operation.
- **CI** builds with GCC and Clang, and runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer.

## Baseline results (v0)

Apple M4, macOS 26, Apple clang 17, `-O3`. Synthetic workload: 55% limit adds within ±50 ticks of mid (10% of them crossing), 40% cancels and 5% market orders. Fixed seed.

| Benchmark | Result |
|---|---|
| Mixed workload, 1M ops | **19.0 M ops/s** (52.7 ms) |
| Mixed workload, 100k ops | 23.9 M ops/s |
| Passive adds only, 100k | 35.0 M ops/s |
| Add + cancel pair, warm book | **33 ns** per pair |

| Operation | p50 | p99 | p99.9 |
|---|---|---|---|
| add_limit | ≤42 ns* | 125 ns | 750 ns |
| cancel | ≤42 ns* | 333 ns | 667 ns |
| add_market | ≤42 ns* | 167 ns | 416 ns |

\* Apple Silicon's timer ticks every ~41.7 ns, so p50 values are below one tick and can't be resolved. Tail values are real but quantised. Nanosecond-level percentiles will come from Linux runs with a TSC-based timer.

## Design notes

| Decision | Why | Planned change |
|---|---|---|
| `std::map` per side for price levels | Simple, ordered, and makes a fair baseline | Flat array indexed by tick offset from a reference price (prices cluster near the mid) |
| `std::unordered_map` for id → slot | Simple | Open-addressing table, or ids that index the pool directly |
| Intrusive list inside a pool | O(1) cancel, no per-order allocation | Keep. Measure the order struct layout (now 32 bytes) for cache fit |
| Trades appended to a caller-owned vector | No allocation once it has warmed up | Template callback, so trades can stream straight to the strategy |

Not handled yet: self-trade prevention, IOC/FOK order types, multiple instruments.

## Roadmap

- [x] **Week 1:** order book core, unit tests, differential test against a reference book, CMake, CI (GCC + Clang, ASan + UBSan)
- [ ] **Week 2:** feed handler. Binance public depth WebSocket → normalise → record to binary → replay through the book
- [ ] **Week 3:** replace `std::map` and `unordered_map`; profile with `perf` and Instruments; publish a before/after table
- [ ] **Week 4:** lock-free SPSC ring buffer between the feed and engine threads; pin threads to cores; re-measure
- [ ] **Week 5:** market-making strategy on replayed data: inventory-skewed quotes, P&L, fill rate
- [ ] **Week 6:** Prometheus + Grafana latency dashboard; one-command Docker Compose; write-up post
- [ ] **Stretch:** Nasdaq ITCH 5.0 binary feed handler and book builder

## Layout

```
include/lob/   public headers (types.hpp, order_book.hpp)
src/           order book implementation
tests/         GoogleTest unit and randomised tests
bench/         workload generator, throughput benchmarks, latency percentiles
```
