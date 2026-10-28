# lob-engine

A limit order book and matching engine in C++20. Replacing `std::map` price levels with flat arrays and a bitset took the mixed workload from **20.1M to 34.0M ops/s (1.69×)**. The same differential test checks both versions, and it catches every bug I've planted on purpose.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/lob_tests && ./build/lob_bench && ./build/lob_latency
```

## Results: v0 (`std::map`) vs v1 (flat array + bitset)

Apple M4, macOS 26, Apple clang 17, `-O3`, median of 3 repetitions. Synthetic workload with a fixed seed: 55% limit adds within ±50 ticks of mid (10% of them crossing), 40% cancels, 5% market orders. Both versions are in this repo, and `lob_bench` runs them side by side.

| Benchmark | v0 `MapOrderBook` | v1 `OrderBook` | Change |
|---|---|---|---|
| Mixed workload, 1M ops | 20.1M ops/s | **34.0M ops/s** | 1.69× |
| Mixed workload, 100k ops | 24.2M ops/s | 40.7M ops/s | 1.68× |
| Passive adds, 100k | 35.2M ops/s | 39.5M ops/s | 1.12× |
| Add + cancel, warm book | 32.5 ns | 28.5 ns | 1.14× |
| Sparse book, 1,000-tick gaps | 2.0 µs | 1.85 µs | 1.08× |

### The step that made it worse first

Flat arrays alone weren't a clear win. When the best level empties, the next one has to be found, and the first v1 walked the array one level at a time. The sparse-book benchmark exposed two problems with that:

| Sparse sweep (50 levels) | v0 map | v1 array, linear scan | v1 + bitset |
|---|---|---|---|
| gap 1 tick | 2.0 µs | 22.5 µs → 1.5 µs* | 1.5 µs |
| gap 100 ticks | 2.0 µs | 3.0 µs | 1.6 µs |
| gap 1,000 ticks | 2.0 µs | 14.8 µs | **1.85 µs** |

\* The 22.5 µs was a bug, not the gap. When the last ask was cleared, the scan walked the whole 65,536-level price band looking for an ask that didn't exist. The level count already knew the side was empty; now it says so and skips the scan.

The fix for real gaps is one bit per price level. Finding the next non-empty level then checks 64 levels per instruction (`std::countr_zero`), which beats following `std::map` pointers even with 1,000 empty ticks between levels.

### Latency percentiles: read with care

On Apple Silicon `steady_clock` ticks every ~41.7 ns, so every sample below is a whole number of ticks; the tool detects this and says so. Treat these as tick counts. Precise nanosecond percentiles will come from Linux runs.

| `lob_latency`, 1M ops | v0 p99 | v1 p99 | v0 p99.9 | v1 p99.9 |
|---|---|---|---|---|
| add_limit | 3 ticks (125 ns) | **2 ticks (84 ns)** | 15 ticks | **3 ticks** |
| cancel | 5 ticks (208 ns) | **2 ticks (84 ns)** | 8 ticks | 5 ticks |

## Correctness

- **Every test runs against both books.** The test suites are typed: the same 22 tests run on `OrderBook` and `MapOrderBook`, plus 3 tests specific to the array (price-band edges, out-of-range rejection, finding the next best price across gaps). 47 tests in total.
- **Differential test:** 20,000 random operations (adds, cancels, three kinds of modify, market orders) go through each book and a deliberately naive [`ReferenceBook`](tests/reference_book.hpp). After every step, trades and book state must match exactly.
- **Mutation-checked:** I planted 12 bugs by hand and the tests caught all 12.
  - Matching rules: priority kept on a size increase, market orders obeying a limit, trades at the taker's price, crossing one tick early, a stale level total.
  - Array book: a skipped rescan (it hangs, which the run's time limit catches), a rescan that skips a level, a new best bid ignored, a level count that never drops.
  - Bitset: a mask off by one, a search starting one level too far, a bit never cleared.
- **CI:** GCC and Clang on Ubuntu, tests under AddressSanitizer and UndefinedBehaviorSanitizer, and UBSan on macOS.

## Design

| Piece | v1 choice | Why |
|---|---|---|
| Price levels | Two flat arrays (bids, asks) indexed by tick offset | Real exchanges enforce price bands. Indexing is one subtraction, with no tree to walk |
| Next best level | A bitset of non-empty levels, scanned with `countr_zero` / `countl_zero` | Checks 64 levels per step, so sparse books stay fast |
| Orders | Pre-allocated pool, intrusive doubly linked list per level | No allocation per order, and O(1) cancel |
| id → order | `std::unordered_map` | **Next to replace:** it's now the main remaining cost on cancel |
| Trades | Appended to a caller-owned vector | No allocation once it has warmed up |

Trade-offs: a 65,536-tick price band costs about 1.5 MB of level arrays per book, and prices outside the band are rejected (`Result::PriceOutOfRange`). Not handled yet: self-trade prevention, IOC/FOK, multiple instruments.

## Roadmap

- [x] Order book core, typed tests, differential test, mutation checks, CI
- [x] **v1:** flat price-level arrays + bitset (1.69× on the mixed workload)
- [ ] Open-addressing id table to replace `unordered_map`
- [ ] Binance depth feed: record to binary, replay through the book, publish results on real data
- [ ] Linux runs with a TSC-based timer for nanosecond percentiles, plus `perf stat` cache-miss numbers
- [ ] SPSC ring buffer between feed and engine threads, with ThreadSanitizer in CI and `rigtorp/SPSCQueue` as the baseline
- [ ] Market-making strategy on replayed data

## Layout

```
include/lob/   order_book.hpp (v1), map_order_book.hpp (v0 baseline), types.hpp
src/           implementations
tests/         typed GoogleTest suites, reference_book.hpp (test oracle)
bench/         workload generator, throughput benchmarks, latency percentiles
```

MIT licensed.
