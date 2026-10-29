// Per-operation latency percentiles over the synthetic workload, for the
// current OrderBook and the v0 MapOrderBook baseline.
//
//   ./lob_latency [ops] [--csv out.csv]   default 1,000,000 ops; --csv writes
//                                        1,001 quantiles per operation for plotting
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

// Writes 1,001 quantiles (0.0%, 0.1%, ... 100%) of one sample set as CSV rows.
void write_quantiles(std::FILE* f, const char* book, const char* op, std::vector<std::uint32_t> v) {
    std::sort(v.begin(), v.end());
    for (int q = 0; q <= 1000; ++q) {
        const std::size_t k = static_cast<std::size_t>((static_cast<double>(q) / 1000.0) * static_cast<double>(v.size() - 1));
        std::fprintf(f, "%s,%s,%.3f,%u\n", book, op, q / 1000.0, v[k]);
    }
}

template <typename B>
void run(const char* label, const char* key, const std::vector<Op>& ops, std::size_t capacity, std::FILE* csv) {
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
    if (csv)
        for (auto* s : {&add, &cancel, &market}) write_quantiles(csv, key, s->name, s->ns);
    std::printf("  resting orders at end: %zu (bid levels %zu, ask levels %zu)\n\n", book.order_count(),
                book.level_count(Side::Buy), book.level_count(Side::Sell));
}

}  // namespace

int main(int argc, char** argv) {
    WorkloadConfig cfg;
    // Usage: lob_latency [ops] [--csv quantiles.csv]
    std::FILE* csv = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--csv" && i + 1 < argc) {
            csv = std::fopen(argv[++i], "w");
            if (!csv) { std::perror("--csv"); return EXIT_FAILURE; }
            std::fprintf(csv, "book,op,quantile,ns\n");
        } else {
            cfg.ops = std::stoul(a);
        }
    }
    const auto ops = make_workload(cfg);

    const std::uint32_t res = clock_resolution_ns();
    std::printf("ops: %zu   clock resolution: %u ns   clock overhead (p50): %u ns\n", cfg.ops, res,
                clock_overhead_ns());
    if (res > 20)
        std::printf("warning: coarse clock; every sample is a multiple of ~%u ns, so these are tick counts,\n"
                    "         not precise latencies. Use the throughput benchmark, or run on Linux.\n",
                    res);
    std::printf("\n");

    // The workload peaks at ~109k live orders; size both books for 256k so the
    // id table is not sized for (and spread across) a million slots.
    constexpr std::size_t kCapacity = 1 << 18;
    run<MapOrderBook>("v0 MapOrderBook (std::map levels)", "v0_map", ops, kCapacity, csv);
    run<OrderBook>("v2 OrderBook (array + bitset levels, open-addressing id table)", "v2", ops, kCapacity, csv);
    if (csv) std::fclose(csv);
    return EXIT_SUCCESS;
}
