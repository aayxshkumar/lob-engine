# lob-engine

A limit order book and matching engine in C++20. Two measured steps took the mixed workload from **21.2M to 67.5M ops/s (3.2×)**: flat price-level arrays with a bitset (v1), then an open-addressing id table in place of `std::unordered_map` (v2). On 20 minutes of real Binance BTCUSDT order flow, v2 is **5.2× faster** than the `std::map` version. The same differential test checks every version, and it catches every bug I've planted on purpose.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/lob_tests && ./build/lob_bench && ./build/lob_latency
```

## Results

Apple M4, macOS 26, Apple clang 17, `-O3`, median of 3 repetitions. Synthetic workload with a fixed seed: 55% limit adds within ±50 ticks of mid (10% of them crossing), 40% cancels, 5% market orders. It peaks at about 109k live orders, and books are sized for 256k unless noted. v0 (`MapOrderBook`) and the current book are both in this repo, and `lob_bench` runs them side by side; v1 numbers are from commit `536cb05`.

| Benchmark | v0 `std::map` | v1 arrays + bitset | v2 + id table | v0 → v2 |
|---|---|---|---|---|
| Mixed workload, 1M ops | 21.2M ops/s | 34.3M ops/s | **67.5M ops/s** | 3.2× |
| Same, random-looking order ids | 20.0M ops/s | 30.4M ops/s | **60.1M ops/s** | 3.0× |
| Passive adds, 100k | 35.6M ops/s | 39.5M ops/s | 158M ops/s | 4.4× |
| Add + cancel, warm book | 32.6 ns | 28.5 ns | **8.6 ns** | 3.8× |
| Sparse book, 1,000-tick gaps | 2.0 µs | 1.85 µs | 1.28 µs | 1.6× |

### v2: the id table, and the benchmark that flattered `std::unordered_map`

Every add, cancel and fill looks an order up by id. After v1, that lookup was the largest cost left: `std::unordered_map` allocates a node per order, and each lookup follows a bucket pointer and then a node pointer.

Before replacing it I made the benchmark harder. The generator handed out ids 1, 2, 3…, which is unusually kind to `unordered_map`: consecutive ids land in consecutive buckets, and nodes allocated in order sit next to each other in memory. Real venues send ids that look random. Scrambling the same ids with a bijective mixer cost v1 25% (34.4M → 25.7M ops/s, book sized for 1M), all of it in the map.

[`IdTable`](include/lob/id_table.hpp) is a flat array of 16-byte `{id, slot}` entries with linear probing, at most half full, using Fibonacci hashing (one multiply). Deletes use backward shift rather than tombstones, so a long-running book doesn't fill up with dead entries. Its capacity is fixed by the order pool, so it never rehashes.

The catch is that a fixed table's memory follows the configured capacity, not the live order count. Sized for 1M orders it's 32 MB, which doesn't fit in cache, and with sequential ids the gain disappears (34.4M → 34.4M). Sized to the workload (256k slots, 8 MB) it's about 2× on both kinds of id. So size the book for the peak you expect, not the largest number you can think of.

### v1: the step that made it worse first

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

| `lob_latency`, 1M ops | v0 p99 | v2 p99 | v0 p99.9 | v2 p99.9 |
|---|---|---|---|---|
| add_limit | 2 ticks | 2 ticks | 14 ticks | **3 ticks** |
| cancel | 3 ticks | **2 ticks** | 6 ticks | **3 ticks** |

The CDFs are in [`docs/`](docs), plotted from [`m4-quantiles.csv`](docs/m4-quantiles.csv) by `tools/plot_cdf.py`.

## On real data: 20 minutes of Binance BTCUSDT

[`tools/record_binance.py`](tools/record_binance.py) records Binance's public depth-diff and trade streams using only the Python standard library. It follows Binance's procedure for keeping a local book (buffer the diffs, take a REST snapshot, drop what the snapshot already covers), and it stops on any sequence gap rather than record a book that disagrees with the exchange.

Binance publishes price levels, not individual orders, so [`lob_replay`](bench/replay.cpp) translates the recording into order operations:

- a level that grows gets a new resting order for the difference;
- a level that shrinks loses size from its newest orders first;
- a trade becomes a taker order at the trade price, so it fills the oldest orders, and any remainder is cancelled;
- within one diff message, reductions are applied before additions.

After every diff message it compares the book's top 10 levels on each side with the exchange's.

Recorded on 29 October 2026, 02:30–02:50 UTC: 11,992 diff messages (544,092 level updates) and 71,328 trades. The 65,536-tick price band (±$327 around $79,900) skipped 44,015 updates to levels further out.

| Replay | |
|---|---|
| Order operations | 562,850: 53.5% adds, 40.3% cancels, 6.2% size reductions |
| Peak resting orders | 4,203 |
| Top 10 levels match the exchange | after 99.93% of diff messages (100% with trades left out) |
| v0 `std::map`, median of 5 | 15.9M ops/s |
| v2, median of 5 | **81.9M ops/s (5.2×)** |

The 0.07% comes from the two streams, not the book. Binance doesn't sequence trades against depth diffs, so a trade can arrive after the diff that already includes it, and the book takes that size twice until the next diff for the level corrects it. Replaying the same recording without trades matches after every message.

Caveats: the order ids are invented, queue positions are inferred from level sizes, and a book of 4,203 orders fits in cache, so this says little about a much deeper book. Recordings aren't committed (`data/` is git-ignored). To make your own:

```bash
python3 tools/record_binance.py BTCUSDT 1200 data/btcusdt.lobrec
./build/lob_replay data/btcusdt.lobrec
```

## Correctness

- **Every test runs against both books.** The test suites are typed: the same 22 tests run on `OrderBook` and `MapOrderBook`, plus 3 tests specific to the array (price-band edges, out-of-range rejection, finding the next best price across gaps) and 4 for the id table. 51 tests in total.
- **Differential test:** 20,000 random operations (adds, cancels, three kinds of modify, market orders) go through each book and a deliberately naive [`ReferenceBook`](tests/reference_book.hpp). After every step, trades and book state must match exactly.
- **Id table differential test:** 20 seeds × 20,000 inserts and erases against `std::unordered_map`, in a 16-entry table so probe runs are long and wrap past the end of the array, which is where backward-shift bugs hide.
- **Mutation-checked:** I planted 16 bugs by hand and the tests caught all 16.
  - Matching rules: priority kept on a size increase, market orders obeying a limit, trades at the taker's price, crossing one tick early, a stale level total.
  - Array book: a skipped rescan (it hangs, which the run's time limit catches), a rescan that skips a level, a new best bid ignored, a level count that never drops.
  - Bitset: a mask off by one, a search starting one level too far, a bit never cleared.
  - Id table: erase without the backward shift, a shift that ignores wrap-around, an off-by-one in the "can this entry stay" test, a shift that stops after one move. (A fifth mutant survived, but it was equivalent: swapping the empty check and the key check can't change the result, because an empty entry's value is already the "absent" marker.)
- **Replay check:** `lob_replay --strict` replays a synthetic recording (`tools/make_test_recording.py`) and fails unless both books end identical and the top 10 levels per side match the recording after every message. It caught a real translation bug: a message that clears an ask and adds a bid at the same price must apply the reduction first, or the bid crosses the stale ask and invents a trade.
- **CI:** GCC and Clang on Ubuntu, tests and the strict replay under AddressSanitizer and UndefinedBehaviorSanitizer, and UBSan on macOS.

## Design

| Piece | Choice | Why |
|---|---|---|
| Price levels | Two flat arrays (bids, asks) indexed by tick offset | Real exchanges enforce price bands. Indexing is one subtraction, with no tree to walk |
| Next best level | A bitset of non-empty levels, scanned with `countr_zero` / `countl_zero` | Checks 64 levels per step, so sparse books stay fast |
| Orders | Pre-allocated pool, intrusive doubly linked list per level | No allocation per order, and O(1) cancel |
| id → order | Open addressing, linear probing, ≤50% load, backward-shift delete | One hash and usually one cache line, with no allocation per order. Memory is fixed by capacity |
| Trades | Appended to a caller-owned vector | No allocation once it has warmed up |

Trade-offs: a 65,536-tick price band costs about 1.5 MB of level arrays per book, and prices outside the band are rejected (`Result::PriceOutOfRange`). Not handled yet: self-trade prevention, IOC/FOK, multiple instruments.

## Roadmap

- [x] Order book core, typed tests, differential test, mutation checks, CI
- [x] **v1:** flat price-level arrays + bitset (1.69× on the mixed workload)
- [x] **v2:** open-addressing id table (about 2× over v1 on the mixed workload)
- [x] Binance depth feed: record to binary, replay through the book (5.2× over v0 on 20 minutes of BTCUSDT)
- [ ] Linux runs with a TSC-based timer for nanosecond percentiles, plus `perf stat` cache-miss numbers
- [ ] SPSC ring buffer between feed and engine threads, with ThreadSanitizer in CI and `rigtorp/SPSCQueue` as the baseline
- [ ] Market-making strategy on replayed data

## Layout

```
include/lob/   order_book.hpp, id_table.hpp, map_order_book.hpp (v0 baseline), types.hpp
src/           implementations
tests/         typed GoogleTest suites, reference_book.hpp (test oracle)
bench/         workload generator, throughput benchmarks, latency percentiles, replay
tools/         record_binance.py (stdlib-only recorder), make_test_recording.py, plot_cdf.py
```

MIT licensed.
