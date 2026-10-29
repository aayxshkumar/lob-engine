#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/id_table.hpp"
#include "lob/types.hpp"

namespace lob {

// Single-instrument limit order book with price-time priority.
//
// v1 layout:
//   - price levels in two flat arrays (bids, asks) indexed by tick offset from
//     a fixed minimum price. Exchanges enforce price bands, so a bounded tick
//     range is realistic; prices outside it are rejected.
//   - the best bid / best ask index is cached. When the best level empties,
//     a bitset of non-empty levels finds the next one 64 levels at a time,
//     so sparse books stay fast too.
//   - orders live in a pre-allocated pool and are chained per level in an
//     intrusive doubly linked list, so cancel is O(1) once the order is found
//   - id -> pool slot lookup through IdTable, a flat open-addressing table
//     sized from the pool (std::unordered_map until v2)
//
// v0 (std::map levels) is kept as MapOrderBook for comparison.
class OrderBook {
public:
    static constexpr Price kDefaultMinPrice = 0;
    static constexpr Price kDefaultMaxPrice = 65'535;

    explicit OrderBook(std::size_t capacity = 1 << 20, Price min_price = kDefaultMinPrice,
                       Price max_price = kDefaultMaxPrice);

    // Limit order: matches against the opposite side while prices cross,
    // then rests any remainder. Trades are appended to `trades`.
    Result add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& trades);

    // Market order: matches until filled or the opposite side is empty.
    // Any unfilled remainder is discarded (never rests).
    Result add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades);

    Result cancel(OrderId id);

    // Reducing quantity at the same price keeps time priority.
    // Changing price or increasing quantity loses it (cancel + re-add),
    // which can trade if the new price crosses.
    Result modify(OrderId id, Price new_price, Qty new_qty, std::vector<Trade>& trades);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Total resting quantity at a price on one side (0 if no level).
    Qty depth_at(Side side, Price price) const;
    std::size_t level_count(Side side) const { return side == Side::Buy ? bid_levels_ : ask_levels_; }
    std::size_t order_count() const { return index_.size(); }

private:
    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

    struct Order {
        OrderId id;
        Price price;
        Qty qty;
        Side side;
        std::uint32_t prev;
        std::uint32_t next;
    };

    struct Level {
        std::uint32_t head = kNil;
        std::uint32_t tail = kNil;
        Qty total = 0;
    };

    bool in_range(Price p) const { return p >= min_price_ && p <= max_price_; }
    std::size_t to_index(Price p) const { return static_cast<std::size_t>(p - min_price_); }
    Level& level(Side side, std::size_t i) { return side == Side::Buy ? bids_[i] : asks_[i]; }

    void match(OrderId taker, Side taker_side, Price limit, bool is_market, Qty& remaining,
               std::vector<Trade>& trades);
    void rest(OrderId id, Side side, Price price, Qty qty);
    void unlink(std::uint32_t slot);
    void level_emptied(Side side, std::size_t i);

    std::uint32_t alloc_slot();
    void free_slot(std::uint32_t slot);

    // One bit per price level, set while the level has orders. Finding the
    // next non-empty level is then a word-at-a-time scan (64 levels per step)
    // instead of a level-at-a-time walk, which keeps sparse books fast.
    static std::int64_t next_set_at_or_above(const std::vector<std::uint64_t>& bits, std::int64_t from);
    static std::int64_t next_set_at_or_below(const std::vector<std::uint64_t>& bits, std::int64_t from);
    static void set_bit(std::vector<std::uint64_t>& bits, std::size_t i) { bits[i >> 6] |= 1ull << (i & 63); }
    static void clear_bit(std::vector<std::uint64_t>& bits, std::size_t i) { bits[i >> 6] &= ~(1ull << (i & 63)); }

    Price min_price_;
    Price max_price_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::vector<std::uint64_t> bid_bits_;
    std::vector<std::uint64_t> ask_bits_;
    // Best indices; "none" is -1 for bids and levels_count for asks.
    std::int64_t best_bid_ = -1;
    std::int64_t best_ask_;
    std::size_t bid_levels_ = 0;
    std::size_t ask_levels_ = 0;

    std::vector<Order> pool_;
    std::vector<std::uint32_t> free_;
    IdTable index_;
};

}  // namespace lob
