// Replay a recording made by tools/record_binance.py through the order books.
//
//   ./build/lob_replay data/btcusdt.lobrec [--strict]
//
// --strict fails unless the top of book matched after every diff message.
// CI uses it on synthetic recordings (tools/make_test_recording.py), which are
// self-consistent; live recordings can lag by a message when a trade arrives
// on the trade stream before the diff that already includes it.
//
// Binance publishes aggregated levels (L2), not individual orders, so the
// recording is first translated into order-level operations:
//   - a level that grows gets a new resting order for the difference;
//   - a level that shrinks loses size from its newest orders first (cancel,
//     or a size reduction that keeps priority);
//   - a trade becomes a taker limit order at the trade price, with any
//     unfilled remainder cancelled, so it fills against the oldest orders.
// Diffs give each level's new absolute size, so whatever the trades leave
// behind is corrected by the next diff that touches that level. Within one
// diff message, reductions are applied before additions: a message can clear
// an ask and place a bid at the same price, and applying the bid first would
// cross the book and invent a trade.
//
// The translated operations are then timed on fresh books (v0 and current),
// which must end in the same state.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "workload.hpp"

using namespace lob;

namespace {

#pragma pack(push, 1)
struct Record {
    std::uint64_t ts_us;
    std::int64_t price;
    std::uint32_t qty;
    std::uint8_t kind;
    std::uint8_t side;
    std::uint16_t pad;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 24);

enum Kind : std::uint8_t { Snapshot = 0, Diff = 1, TradeRec = 2, DiffEnd = 3 };

enum class OpType : std::uint8_t { Add, Cancel, Reduce };
struct Op {
    OpType type;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

std::vector<Record> load(const char* path, std::int64_t& price_scale, std::int64_t& qty_scale) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); std::exit(EXIT_FAILURE); }
    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "LOBREC01", 8) != 0 ||
        std::fread(&price_scale, 8, 1, f) != 1 || std::fread(&qty_scale, 8, 1, f) != 1) {
        std::fprintf(stderr, "%s: not a LOBREC01 file\n", path);
        std::exit(EXIT_FAILURE);
    }
    std::vector<Record> recs;
    Record r;
    while (std::fread(&r, sizeof r, 1, f) == 1) recs.push_back(r);
    std::fclose(f);
    return recs;
}

// Turns L2 records into order operations, applying them to a book as it goes
// (later decisions depend on what earlier ones filled).
class Translator {
public:
    Translator(Price min_price, Price max_price, std::size_t capacity)
        : book_(capacity, min_price, max_price), min_(min_price), max_(max_price) {}

    void level(Side side, Price price, Qty target) {
        auto& ref = side == Side::Buy ? ref_bids_ : ref_asks_;
        if (target == 0) ref.erase(price); else ref[price] = target;
        if (price < min_ || price > max_) { ++out_of_band_; return; }

        const Qty cur = book_.depth_at(side, price);
        if (target > cur) {
            add(side, price, target - cur);
            if (!trades_.empty()) ++crossings_;  // our book was behind the exchange's
        } else if (target < cur) {
            Qty need = cur - target;
            auto& ids = queue(side, price);
            while (need > 0 && !ids.empty()) {
                const OrderId id = ids.back();
                Qty& q = qty_[id];
                if (q <= need) {
                    need -= q;
                    emit({OpType::Cancel, side, id, price, 0});
                    book_.cancel(id);
                    qty_.erase(id);
                    ids.pop_back();
                } else {
                    q -= need;
                    emit({OpType::Reduce, side, id, price, q});
                    book_.modify(id, price, q, trades_);
                    need = 0;
                }
            }
        }
    }

    void trade(Side aggressor, Price price, Qty qty) {
        if (price < min_ || price > max_) { ++out_of_band_; return; }
        const OrderId id = add(aggressor, price, qty);
        if (qty_.contains(id)) {  // rested: cancel the remainder, like IOC
            emit({OpType::Cancel, aggressor, id, price, 0});
            book_.cancel(id);
            auto& ids = queue(aggressor, price);
            ids.erase(std::find(ids.begin(), ids.end(), id));
            qty_.erase(id);
        }
    }

    void diff(Side side, Price price, Qty target) { pending_.push_back({side, price, target}); }

    // End of one diff message: apply its levels (reductions first), then
    // check that our top of book (10 levels a side) matches the exchange's.
    void end_message() {
        auto shrinks = [&](const Pending& d) {
            return d.price >= min_ && d.price <= max_ && d.target < book_.depth_at(d.side, d.price);
        };
        std::stable_partition(pending_.begin(), pending_.end(), shrinks);
        for (const Pending& d : pending_) level(d.side, d.price, d.target);
        pending_.clear();
        check_top();
    }

    void check_top() {
        ++checks_;
        if (top(Side::Buy) && top(Side::Sell)) ++in_sync_;
    }

    const std::vector<Op>& ops() const { return ops_; }
    std::size_t peak_live() const { return peak_; }
    std::size_t crossings() const { return crossings_; }
    std::size_t out_of_band() const { return out_of_band_; }
    double sync_rate() const { return checks_ ? double(in_sync_) / double(checks_) : 0.0; }

private:
    struct Pending {
        Side side;
        Price price;
        Qty target;
    };

    std::vector<OrderId>& queue(Side side, Price price) {
        return (side == Side::Buy ? bid_q_ : ask_q_)[price];
    }

    void emit(const Op& op) { ops_.push_back(op); }

    OrderId add(Side side, Price price, Qty qty) {
        const OrderId id = bench::scramble(next_++);
        emit({OpType::Add, side, id, price, qty});
        trades_.clear();
        book_.add_limit(id, side, price, qty, trades_);
        Qty filled = 0;
        for (const Trade& t : trades_) {
            filled += t.qty;
            Qty& mq = qty_[t.maker_id];
            mq -= t.qty;
            if (mq == 0) {
                qty_.erase(t.maker_id);
                auto& ids = queue(opposite(side), t.price);
                ids.erase(std::find(ids.begin(), ids.end(), t.maker_id));
            }
        }
        if (filled < qty) {
            qty_[id] = qty - filled;
            queue(side, price).push_back(id);
        }
        peak_ = std::max(peak_, book_.order_count());
        return id;
    }

    bool top(Side side) {
        const auto& ref = side == Side::Buy ? ref_bids_ : ref_asks_;
        auto best = side == Side::Buy ? book_.best_bid() : book_.best_ask();
        int n = 0;
        auto check = [&](auto it, auto end) {
            Price p = best ? *best : 0;
            for (; it != end && n < 10; ++it, ++n) {
                if (it->first < min_ || it->first > max_) return true;
                if (!best) return false;
                // Walk our book to its next non-empty level.
                while (p >= min_ && p <= max_ && book_.depth_at(side, p) == 0) p += side == Side::Buy ? -1 : 1;
                if (p != it->first || book_.depth_at(side, p) != it->second) return false;
                p += side == Side::Buy ? -1 : 1;
            }
            return true;
        };
        return side == Side::Buy ? check(ref.rbegin(), ref.rend()) : check(ref.begin(), ref.end());
    }

    OrderBook book_;
    Price min_, max_;
    std::vector<Trade> trades_;
    std::vector<Op> ops_;
    std::vector<Pending> pending_;
    std::unordered_map<OrderId, Qty> qty_;
    std::map<Price, std::vector<OrderId>> bid_q_, ask_q_;
    std::map<Price, Qty> ref_bids_, ref_asks_;
    OrderId next_ = 1;
    std::size_t peak_ = 0, crossings_ = 0, out_of_band_ = 0, checks_ = 0, in_sync_ = 0;
};

struct RunResult {
    double seconds;
    std::size_t trades;
    std::size_t live;
    std::optional<Price> bid, ask;
};

template <typename B>
RunResult time_ops(B& book, const std::vector<Op>& ops, std::vector<Trade>& trades, std::size_t& n_trades) {
    const auto t0 = std::chrono::steady_clock::now();
    for (const Op& op : ops) {
        switch (op.type) {
            case OpType::Add: book.add_limit(op.id, op.side, op.price, op.qty, trades); break;
            case OpType::Cancel: book.cancel(op.id); break;
            case OpType::Reduce: book.modify(op.id, op.price, op.qty, trades); break;
        }
        n_trades += trades.size();
        trades.clear();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return {std::chrono::duration<double>(t1 - t0).count(), n_trades, book.order_count(), book.best_bid(),
            book.best_ask()};
}

template <typename B>
RunResult run(const std::vector<Op>& ops, std::size_t capacity, Price min_price, Price max_price) {
    std::vector<Trade> trades;
    trades.reserve(1024);
    std::size_t n_trades = 0;
    if constexpr (std::is_same_v<B, OrderBook>) {
        B book(capacity, min_price, max_price);
        return time_ops(book, ops, trades, n_trades);
    } else {
        B book(capacity);
        return time_ops(book, ops, trades, n_trades);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    bool strict = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--strict") == 0) strict = true;
        else path = argv[i];
    }
    if (!path) {
        std::fprintf(stderr, "usage: lob_replay recording.lobrec [--strict]\n");
        return EXIT_FAILURE;
    }
    std::int64_t price_scale = 0, qty_scale = 0;
    const auto recs = load(path, price_scale, qty_scale);

    // Centre the 65,536-tick band on the snapshot's mid price.
    Price best_bid = 0, best_ask = 0;
    for (const Record& r : recs) {
        if (r.kind != Snapshot) break;
        if (r.side == 0) best_bid = std::max(best_bid, r.price);
        else best_ask = best_ask ? std::min(best_ask, r.price) : r.price;
    }
    const Price mid = (best_bid + best_ask) / 2;
    const Price lo = mid - 32'768, hi = mid + 32'767;

    std::size_t n_snapshot = 0, n_diff = 0, n_msgs = 0, n_trade_recs = 0;
    Translator tr(lo, hi, 1 << 21);
    for (const Record& r : recs) {
        const Side side = r.side == 0 ? Side::Buy : Side::Sell;
        switch (r.kind) {
            case Snapshot: ++n_snapshot; tr.level(side, r.price, r.qty); break;
            case Diff: ++n_diff; tr.diff(side, r.price, r.qty); break;
            case TradeRec: ++n_trade_recs; tr.trade(side, r.price, r.qty); break;
            case DiffEnd: ++n_msgs; tr.end_message(); break;
        }
    }
    const auto& ops = tr.ops();
    std::size_t adds = 0, cancels = 0, reduces = 0;
    for (const Op& op : ops) {
        adds += op.type == OpType::Add;
        cancels += op.type == OpType::Cancel;
        reduces += op.type == OpType::Reduce;
    }
    const double secs = recs.size() > 1 ? double(recs.back().ts_us - recs.front().ts_us) / 1e6 : 0.0;

    std::printf("recording: %zu snapshot levels, %zu diff messages (%zu level updates), %zu trades, %.0f s\n",
                n_snapshot, n_msgs, n_diff, n_trade_recs, secs);
    std::printf("price band: %.2f to %.2f (%zu updates outside it skipped)\n", double(lo) / double(price_scale),
                double(hi) / double(price_scale), tr.out_of_band());
    std::printf("translated: %zu ops = %zu adds (%.1f%%), %zu cancels (%.1f%%), %zu size reductions (%.1f%%)\n",
                ops.size(), adds, 100.0 * double(adds) / double(ops.size()), cancels,
                100.0 * double(cancels) / double(ops.size()), reduces, 100.0 * double(reduces) / double(ops.size()));
    std::printf("peak live orders: %zu   level updates that crossed our book: %zu\n", tr.peak_live(), tr.crossings());
    std::printf("top 10 levels match the exchange after %.2f%% of diff messages\n\n", 100.0 * tr.sync_rate());
    if (strict && tr.sync_rate() < 1.0) {
        std::fprintf(stderr, "--strict: top of book diverged from the recording\n");
        return EXIT_FAILURE;
    }

    const std::size_t capacity = std::bit_ceil(tr.peak_live() * 2);
    constexpr int kReps = 5;
    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    std::vector<double> t0s, t2s;
    RunResult r0{}, r2{};
    for (int i = 0; i < kReps; ++i) {
        r0 = run<MapOrderBook>(ops, capacity, lo, hi);
        r2 = run<OrderBook>(ops, capacity, lo, hi);
        t0s.push_back(r0.seconds);
        t2s.push_back(r2.seconds);
    }
    if (r0.trades != r2.trades || r0.live != r2.live || r0.bid != r2.bid || r0.ask != r2.ask) {
        std::fprintf(stderr, "MISMATCH between books: trades %zu vs %zu, live %zu vs %zu\n", r0.trades, r2.trades,
                     r0.live, r2.live);
        return EXIT_FAILURE;
    }
    const double m0 = median(t0s), m2 = median(t2s);
    std::printf("replay, median of %d (capacity %zu):\n", kReps, capacity);
    std::printf("  v0 MapOrderBook  %8.2f ms  %6.1fM ops/s\n", m0 * 1e3, double(ops.size()) / m0 / 1e6);
    std::printf("  v2 OrderBook     %8.2f ms  %6.1fM ops/s  (%.2fx)\n", m2 * 1e3, double(ops.size()) / m2 / 1e6,
                m0 / m2);
    std::printf("  both books: %zu fills, %zu orders resting at the end\n", r2.trades, r2.live);
    return EXIT_SUCCESS;
}
