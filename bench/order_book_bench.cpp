// Throughput benchmarks (Google Benchmark). For per-operation latency
// percentiles, see latency.cpp.

#include <benchmark/benchmark.h>

#include <chrono>
#include <optional>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;

namespace {

template <typename B>
void apply(B& book, const Op& op, std::vector<Trade>& trades) {
    switch (op.type) {
        case OpType::Add: book.add_limit(op.id, op.side, op.price, op.qty, trades); break;
        case OpType::Cancel: book.cancel(op.id); break;
        case OpType::Market: book.add_market(op.id, op.side, op.qty, trades); break;
    }
}

// Full mixed workload: adds, cancels, crossing orders and market orders.
template <typename B, bool RandomIds = false>
void BM_MixedWorkload(benchmark::State& state) {
    WorkloadConfig cfg;
    cfg.ops = static_cast<std::size_t>(state.range(0));
    cfg.random_ids = RandomIds;
    cfg.capacity = static_cast<std::size_t>(state.range(1));
    const auto ops = make_workload(cfg);
    std::vector<Trade> trades;
    trades.reserve(1024);

    std::optional<B> book;
    for (auto _ : state) {
        state.PauseTiming();
        book.reset();  // the last book's destructor runs untimed
        book.emplace(cfg.capacity ? cfg.capacity : cfg.ops);
        state.ResumeTiming();
        for (const Op& op : ops) {
            apply(*book, op, trades);
            trades.clear();
        }
        benchmark::DoNotOptimize(book->best_bid());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK_TEMPLATE(BM_MixedWorkload, MapOrderBook)->Name("v0_map/mixed")->Args({100'000, 0})->Args({1'000'000, 0})->Args({1'000'000, 1 << 18})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MixedWorkload, OrderBook)->Name("v2/mixed")->Args({100'000, 0})->Args({1'000'000, 0})->Args({1'000'000, 1 << 18})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MixedWorkload, MapOrderBook, true)->Name("v0_map/mixed_random_ids")->Args({1'000'000, 0})->Args({1'000'000, 1 << 18})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MixedWorkload, OrderBook, true)->Name("v2/mixed_random_ids")->Args({1'000'000, 0})->Args({1'000'000, 1 << 18})->Unit(benchmark::kMillisecond);

// Passive adds only (no matching): isolates level lookup + insert cost.
template <typename B>
void BM_AddPassive(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<Trade> trades;
    for (auto _ : state) {
        state.PauseTiming();
        B book{n};
        state.ResumeTiming();
        for (std::size_t i = 0; i < n; ++i) {
            const Side s = (i & 1) ? Side::Buy : Side::Sell;
            const Price px = s == Side::Buy ? 10'000 - Price(i % 50) - 1 : 10'000 + Price(i % 50) + 1;
            book.add_limit(i + 1, s, px, 10, trades);
        }
        benchmark::DoNotOptimize(book.order_count());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK_TEMPLATE(BM_AddPassive, MapOrderBook)->Name("v0_map/add_passive")->Arg(100'000)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_AddPassive, OrderBook)->Name("v2/add_passive")->Arg(100'000)->Unit(benchmark::kMillisecond);

// Add then cancel the same order: the hot path for quote updates.
template <typename B>
void BM_AddCancel(benchmark::State& state) {
    B book{1024};
    std::vector<Trade> trades;
    // Some resting depth so levels exist.
    for (OrderId id = 1; id <= 200; ++id) book.add_limit(id, Side::Buy, 9'900 + Price(id % 50), 10, trades);
    OrderId id = 1'000'000;
    for (auto _ : state) {
        book.add_limit(id, Side::Buy, 9'925, 10, trades);
        book.cancel(id);
        ++id;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK_TEMPLATE(BM_AddCancel, MapOrderBook)->Name("v0_map/add_cancel");
BENCHMARK_TEMPLATE(BM_AddCancel, OrderBook)->Name("v2/add_cancel");

// Worst case for flat arrays: a sparse book. Asks sit `gap` ticks apart and a
// market order clears the best one each time, so the array version has to
// scan `gap` empty levels to find the next best. std::map just takes the
// next node.
template <typename B>
void BM_SparseSweep(benchmark::State& state) {
    const Price gap = static_cast<Price>(state.range(0));
    constexpr int kLevels = 50;
    std::vector<Trade> trades;
    trades.reserve(16);
    OrderId id = 1;
    for (auto _ : state) {
        B book{1024};
        for (int i = 0; i < kLevels; ++i) book.add_limit(id++, Side::Sell, 1'000 + i * gap, 1, trades);
        // Time only the sweep: a PauseTiming/ResumeTiming pair costs ~0.4 us on an M4, about as much
        // as the sweep, and v2's destructor (two 65,536-level arrays) runs after the clock stops.
        // Each sweep is 10-40 clock ticks; the call's phase against the clock is random, so the
        // mean is unbiased.
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kLevels; ++i) book.add_market(id++, Side::Buy, 1, trades);
        benchmark::DoNotOptimize(book.best_ask());
        const auto t1 = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
        trades.clear();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * kLevels);
}
BENCHMARK_TEMPLATE(BM_SparseSweep, MapOrderBook)->Name("v0_map/sparse_sweep")->Arg(1)->Arg(100)->Arg(1'000)->UseManualTime();
BENCHMARK_TEMPLATE(BM_SparseSweep, OrderBook)->Name("v2/sparse_sweep")->Arg(1)->Arg(100)->Arg(1'000)->UseManualTime();

}  // namespace

BENCHMARK_MAIN();
