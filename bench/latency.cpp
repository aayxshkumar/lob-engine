// Per-operation latency percentiles over the synthetic workload.
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

// Clock overhead, measured the same way as the real samples.
std::uint32_t clock_overhead_ns() {
    std::vector<std::uint32_t> v(100'000);
    for (auto& x : v) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        x = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
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
        best = std::min(best, static_cast<std::uint32_t>(
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    WorkloadConfig cfg;
    if (argc > 1) cfg.ops = std::stoul(argv[1]);
    const auto ops = make_workload(cfg);

    OrderBook book{cfg.ops};
    std::vector<Trade> trades;
    trades.reserve(1024);

    Samples add{"add_limit", {}}, cancel{"cancel", {}}, market{"add_market", {}};
    for (auto* s : {&add, &cancel, &market}) s->ns.reserve(cfg.ops);

    for (const Op& op : ops) {
        const auto t0 = Clock::now();
        switch (op.type) {
            case OpType::Add: book.add_limit(op.id, op.side, op.price, op.qty, trades); break;
            case OpType::Cancel: book.cancel(op.id); break;
            case OpType::Market: book.add_market(op.id, op.side, op.qty, trades); break;
        }
        const auto t1 = Clock::now();
        const auto ns = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        (op.type == OpType::Add ? add : op.type == OpType::Cancel ? cancel : market).ns.push_back(ns);
        trades.clear();
    }

    const std::uint32_t res = clock_resolution_ns();
    std::printf("ops: %zu   clock resolution: %u ns   clock overhead (p50): %u ns\n", cfg.ops, res,
                clock_overhead_ns());
    if (res > 20)
        std::printf("warning: coarse clock; values below ~%u ns are timer ticks, not real latency.\n"
                    "         Use the throughput benchmark for averages, or run on Linux for ns-level samples.\n",
                    res);
    std::printf("\n");
    std::printf("%-12s %10s %8s %8s %8s %10s\n", "operation", "count", "p50", "p99", "p99.9", "max");
    for (auto* s : {&add, &cancel, &market}) {
        const std::size_t n = s->ns.size();
        const auto mx = n ? *std::max_element(s->ns.begin(), s->ns.end()) : 0u;
        std::printf("%-12s %10zu %6u ns %6u ns %6u ns %8u ns\n", s->name, n, pct(s->ns, 0.50), pct(s->ns, 0.99),
                    pct(s->ns, 0.999), mx);
    }
    std::printf("\nresting orders at end: %zu  (bid levels %zu, ask levels %zu)\n", book.order_count(),
                book.level_count(Side::Buy), book.level_count(Side::Sell));
    return EXIT_SUCCESS;
}
