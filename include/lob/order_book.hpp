#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Single-instrument limit order book with price-time priority.
//
// Baseline (v0) layout, chosen for clarity, not speed:
//   - price levels in std::map (bids descending, asks ascending)
//   - orders live in a pre-allocated pool and are chained per level in an
//     intrusive doubly linked list, so cancel is O(1) once the order is found
//   - id -> pool slot lookup through std::unordered_map
//
// Week 3 of the roadmap replaces the map and the hash map and measures the
// difference; keep this version around as the baseline for that comparison.
class OrderBook {
public:
    explicit OrderBook(std::size_t capacity = 1 << 20);

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
    std::size_t level_count(Side side) const;
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

    using Bids = std::map<Price, Level, std::greater<Price>>;
    using Asks = std::map<Price, Level, std::less<Price>>;

    template <typename Book>
    void match(Book& book, OrderId taker, Side taker_side, Price limit, bool is_market, Qty& remaining,
               std::vector<Trade>& trades);

    void rest(OrderId id, Side side, Price price, Qty qty);
    void unlink(std::uint32_t slot);

    std::uint32_t alloc_slot();
    void free_slot(std::uint32_t slot);

    std::vector<Order> pool_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<OrderId, std::uint32_t> index_;
    Bids bids_;
    Asks asks_;
};

}  // namespace lob
