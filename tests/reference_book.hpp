#pragma once

// Deliberately naive order book used only as a test oracle: a flat vector
// scanned linearly for the best price and earliest arrival. Slow but simple
// enough to be obviously correct, so OrderBook is checked against it.

#include <algorithm>
#include <optional>
#include <vector>

#include "lob/types.hpp"

namespace lob::test {

class ReferenceBook {
public:
    void add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& trades) {
        Qty remaining = match(id, side, price, false, qty, trades);
        if (remaining > 0) orders_.push_back({id, side, price, remaining, seq_++});
    }

    void add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) {
        match(id, side, 0, true, qty, trades);
    }

    bool cancel(OrderId id) {
        auto it = std::find_if(orders_.begin(), orders_.end(), [&](const O& o) { return o.id == id; });
        if (it == orders_.end()) return false;
        orders_.erase(it);
        return true;
    }

    // Same priority rules as OrderBook::modify: shrinking at the same price
    // keeps queue position; anything else requeues (and may trade).
    bool modify(OrderId id, Price price, Qty qty, std::vector<Trade>& trades) {
        auto it = std::find_if(orders_.begin(), orders_.end(), [&](const O& o) { return o.id == id; });
        if (it == orders_.end()) return false;
        if (qty == 0) {
            orders_.erase(it);
            return true;
        }
        if (price == it->price && qty <= it->qty) {
            it->qty = qty;
            return true;
        }
        const Side side = it->side;
        orders_.erase(it);
        add_limit(id, side, price, qty, trades);
        return true;
    }

    std::optional<Price> best(Side side) const {
        std::optional<Price> b;
        for (const O& o : orders_)
            if (o.side == side && (!b || (side == Side::Buy ? o.price > *b : o.price < *b))) b = o.price;
        return b;
    }

    Qty depth_at(Side side, Price price) const {
        Qty total = 0;
        for (const O& o : orders_)
            if (o.side == side && o.price == price) total += o.qty;
        return total;
    }

    std::size_t order_count() const { return orders_.size(); }

    std::optional<Price> price_of(OrderId id) const {
        for (const O& o : orders_)
            if (o.id == id) return o.price;
        return std::nullopt;
    }

    Qty qty_of(OrderId id) const {
        for (const O& o : orders_)
            if (o.id == id) return o.qty;
        return 0;
    }

private:
    struct O {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };

    Qty match(OrderId taker, Side side, Price limit, bool is_market, Qty qty, std::vector<Trade>& trades) {
        const Side maker_side = opposite(side);
        while (qty > 0) {
            // Best maker: best price, then lowest arrival sequence.
            O* best = nullptr;
            for (O& o : orders_) {
                if (o.side != maker_side) continue;
                if (!best) { best = &o; continue; }
                const bool better = maker_side == Side::Sell ? o.price < best->price : o.price > best->price;
                if (better || (o.price == best->price && o.seq < best->seq)) best = &o;
            }
            if (!best) break;
            if (!is_market && (side == Side::Buy ? best->price > limit : best->price < limit)) break;

            const Qty fill = std::min(qty, best->qty);
            trades.push_back({best->id, taker, best->price, fill});
            qty -= fill;
            best->qty -= fill;
            if (best->qty == 0) {
                const OrderId done = best->id;
                orders_.erase(std::find_if(orders_.begin(), orders_.end(), [&](const O& o) { return o.id == done; }));
            }
        }
        return qty;
    }

    std::vector<O> orders_;
    std::uint64_t seq_ = 0;
};

}  // namespace lob::test
