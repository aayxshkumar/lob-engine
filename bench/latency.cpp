// Per-operation latency percentiles over the synthetic workload, for the
// current OrderBook and the v0 MapOrderBook baseline.
//
//   ./lob_latency [ops]          default 1,000,000
//
// Each operation is timed individually with steady_clock. The clock read
// itself costs tens of nanoseconds, so treat absolute numbers as an upper
// bound; compare versions against each other on the same machine.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;
using Clock = std::chrono::steady_clock;

namespace {

struct Samples {
    const char* name;
    std::vector<std::uint32_t> ns;
};

std::uint32_t pct(std::vector<std::uint32_t>& v, double p) {
    if (v.empty()) return 0;
    const std::size_t k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

std::uint32_t to_ns(Clock::duration d) {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

// Clock overhead, measured the same way as the real samples.
std::uint32_t clock_overhead_ns() {
    std::vector<std::uint32_t> v(100'000);
    for (auto& x : v) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        x = to_ns(t1 - t0);
    }
    return pct(v, 0.5);
}

// Smallest non-zero step the clock can report. Apple Silicon's timer ticks at
// 24 MHz (~41.7 ns), so samples there are quantised to multiples of that.
std::uint32_t clock_resolution_ns() {
    std::uint32_t best = ~0u;
    for (int i = 0; i < 10'000; ++i) {
        const auto t0 = Clock::now();
        auto t1 = Clock::now();
        while (t1 == t0) t1 = Clock::now();
        best = std::min(best, to_ns(t1 - t0));
    }
    return best;
}

template <typename B>
void run(const char* label, const std::vector<Op>& ops, std::size_t capacity) {
    B book{capacity};
    std::vector<Trade> trades;
    trades.reserve(1024);

    Samples add{"add_limit", {}}, cancel{"cancel", {}}, market{"add_market", {}};
    for (auto* s : {&add, &cancel, &market}) s->ns.reserve(ops.size());

    for (const Op& op : ops) {
        const auto t0 = Clock::now();
        switch (op.type) {
            case OpType::Add: book.add_limit(op.id, op.side, op.price, op.qty, trades); break;
            case OpType::Cancel: book.cancel(op.id); break;
            case OpType::Market: book.add_market(op.id, op.side, op.qty, trades); break;
        }
        const auto t1 = Clock::now();
        (op.type == OpType::Add ? add : op.type == OpType::Cancel ? cancel : market).ns.push_back(to_ns(t1 - t0));
        trades.clear();
    }

    std::printf("%s\n", label);
    std::printf("  %-12s %10s %8s %8s %8s %10s\n", "operation", "count", "p50", "p99", "p99.9", "max");
    for (auto* s : {&add, &cancel, &market}) {
        const std::size_t n = s->ns.size();
        const auto mx = n ? *std::max_element(s->ns.begin(), s->ns.end()) : 0u;
        std::printf("  %-12s %10zu %6u ns %6u ns %6u ns %8u ns\n", s->name, n, pct(s->ns, 0.50), pct(s->ns, 0.99),
                    pct(s->ns, 0.999), mx);
    }
    std::printf("  resting orders at end: %zu (bid levels %zu, ask levels %zu)\n\n", book.order_count(),
                book.level_count(Side::Buy), book.level_count(Side::Sell));
}

}  // namespace

int main(int argc, char** argv) {
    WorkloadConfig cfg;
    if (argc > 1) cfg.ops = std::stoul(argv[1]);
    const auto ops = make_workload(cfg);

    const std::uint32_t res = clock_resolution_ns();
    std::printf("ops: %zu   clock resolution: %u ns   clock overhead (p50): %u ns\n", cfg.ops, res,
                clock_overhead_ns());
    if (res > 20)
        std::printf("warning: coarse clock; every sample is a multiple of ~%u ns, so these are tick counts,\n"
                    "         not precise latencies. Use the throughput benchmark, or run on Linux.\n",
                    res);
    std::printf("\n");

    run<MapOrderBook>("v0 MapOrderBook (std::map levels)", ops, cfg.ops);
    run<OrderBook>("v1 OrderBook (flat array levels)", ops, cfg.ops);
    return EXIT_SUCCESS;
}
