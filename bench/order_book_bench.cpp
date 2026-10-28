// Throughput benchmarks (Google Benchmark). For per-operation latency
// percentiles, see latency.cpp.

#include <benchmark/benchmark.h>

#include "lob/order_book.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;

namespace {

void apply(OrderBook& book, const Op& op, std::vector<Trade>& trades) {
    switch (op.type) {
        case OpType::Add: book.add_limit(op.id, op.side, op.price, op.qty, trades); break;
        case OpType::Cancel: book.cancel(op.id); break;
        case OpType::Market: book.add_market(op.id, op.side, op.qty, trades); break;
    }
}

// Full mixed workload: adds, cancels, crossing orders and market orders.
void BM_MixedWorkload(benchmark::State& state) {
    WorkloadConfig cfg;
    cfg.ops = static_cast<std::size_t>(state.range(0));
    const auto ops = make_workload(cfg);
    std::vector<Trade> trades;
    trades.reserve(1024);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book{cfg.ops};
        state.ResumeTiming();
        for (const Op& op : ops) {
            apply(book, op, trades);
            trades.clear();
        }
        benchmark::DoNotOptimize(book.best_bid());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_MixedWorkload)->Arg(100'000)->Arg(1'000'000)->Unit(benchmark::kMillisecond);

// Passive adds only (no matching): isolates level lookup + insert cost.
void BM_AddPassive(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<Trade> trades;
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book{n};
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
BENCHMARK(BM_AddPassive)->Arg(100'000)->Unit(benchmark::kMillisecond);

// Add then cancel the same order: the hot path for quote updates.
void BM_AddCancel(benchmark::State& state) {
    OrderBook book{1024};
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
BENCHMARK(BM_AddCancel);

}  // namespace

BENCHMARK_MAIN();
