#pragma once

// Deterministic synthetic order flow shared by the benchmarks.
//
// Rough shape of real books: most messages are adds and cancels near the top
// of book, a minority cross the spread. Week 2 replaces this with replayed
// Binance depth data; keep this generator for reproducible comparisons.

#include <cstdint>
#include <random>
#include <vector>

#include "lob/types.hpp"

namespace lob::bench {

enum class OpType : std::uint8_t { Add, Cancel, Market };

struct Op {
    OpType type;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

struct WorkloadConfig {
    std::size_t ops = 1'000'000;
    Price mid = 10'000;
    int spread_ticks = 50;     // adds land within +/- this of mid
    double cancel_ratio = 0.40;
    double market_ratio = 0.05;
    std::uint64_t seed = 7;
    // Sequential ids (1, 2, 3...) are kind to any hash table. Real venues
    // send ids that look random, so benchmarks also run with ids scrambled
    // by a bijective mixer (still unique).
    bool random_ids = false;
    // Book capacity for benchmarks; 0 means one slot per op (always enough).
    // The workload peaks at about 109k live orders.
    std::size_t capacity = 0;
};

// splitmix64 finalizer: a bijection on 64-bit integers.
inline std::uint64_t scramble(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline std::vector<Op> make_workload(const WorkloadConfig& cfg) {
    std::mt19937_64 rng(cfg.seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::uniform_int_distribution<int> offset(1, cfg.spread_ticks);
    std::uniform_int_distribution<int> qty(1, 100);

    std::vector<Op> ops;
    ops.reserve(cfg.ops);
    std::vector<OrderId> live;
    live.reserve(cfg.ops);
    OrderId seq = 1;
    auto next_id = [&] { const OrderId id = seq++; return cfg.random_ids ? scramble(id) : id; };

    for (std::size_t i = 0; i < cfg.ops; ++i) {
        const double r = u(rng);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        if (r < cfg.cancel_ratio && !live.empty()) {
            const std::size_t k = rng() % live.size();
            ops.push_back({OpType::Cancel, side, live[k], 0, 0});
            live[k] = live.back();
            live.pop_back();
        } else if (r < cfg.cancel_ratio + cfg.market_ratio) {
            ops.push_back({OpType::Market, side, next_id(), 0, static_cast<Qty>(qty(rng))});
        } else {
            // Mostly passive: buys below mid, sells above, with occasional crossing.
            const int off = offset(rng);
            const bool aggressive = u(rng) < 0.1;
            const Price px = side == Side::Buy ? cfg.mid + (aggressive ? off : -off)
                                               : cfg.mid + (aggressive ? -off : off);
            const OrderId id = next_id();
            ops.push_back({OpType::Add, side, id, px, static_cast<Qty>(qty(rng))});
            live.push_back(id);
        }
    }
    return ops;
}

}  // namespace lob::bench
