#include "lob/map_order_book.hpp"

#include <algorithm>
#include <stdexcept>

namespace lob {

MapOrderBook::MapOrderBook(std::size_t capacity) {
    pool_.resize(capacity);
    free_.reserve(capacity);
    // Hand out low slots first so a fresh book touches memory in order.
    for (std::size_t i = capacity; i-- > 0;) free_.push_back(static_cast<std::uint32_t>(i));
    index_.reserve(capacity);
}

std::uint32_t MapOrderBook::alloc_slot() {
    if (free_.empty()) throw std::length_error("order pool exhausted");
    std::uint32_t slot = free_.back();
    free_.pop_back();
    return slot;
}

void MapOrderBook::free_slot(std::uint32_t slot) { free_.push_back(slot); }

Result MapOrderBook::add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& trades) {
    if (qty == 0) return Result::InvalidQty;
    if (index_.contains(id)) return Result::DuplicateId;

    Qty remaining = qty;
    if (side == Side::Buy)
        match(asks_, id, side, price, false, remaining, trades);
    else
        match(bids_, id, side, price, false, remaining, trades);

    if (remaining > 0) rest(id, side, price, remaining);
    return Result::Ok;
}

Result MapOrderBook::add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) {
    if (qty == 0) return Result::InvalidQty;
    if (index_.contains(id)) return Result::DuplicateId;

    Qty remaining = qty;
    if (side == Side::Buy)
        match(asks_, id, side, 0, true, remaining, trades);
    else
        match(bids_, id, side, 0, true, remaining, trades);
    return Result::Ok;
}

template <typename Book>
void MapOrderBook::match(Book& book, OrderId taker, Side taker_side, Price limit, bool is_market, Qty& remaining,
                      std::vector<Trade>& trades) {
    while (remaining > 0 && !book.empty()) {
        auto it = book.begin();  // best opposite price
        const Price level_price = it->first;
        if (!is_market) {
            const bool crosses = taker_side == Side::Buy ? level_price <= limit : level_price >= limit;
            if (!crosses) break;
        }

        Level& level = it->second;
        while (remaining > 0 && level.head != kNil) {
            const std::uint32_t slot = level.head;
            Order& maker = pool_[slot];
            const Qty fill = std::min(remaining, maker.qty);

            trades.push_back(Trade{maker.id, taker, level_price, fill});
            remaining -= fill;
            maker.qty -= fill;
            level.total -= fill;

            if (maker.qty == 0) {
                level.head = maker.next;
                if (level.head != kNil)
                    pool_[level.head].prev = kNil;
                else
                    level.tail = kNil;
                index_.erase(maker.id);
                free_slot(slot);
            }
        }
        if (level.head == kNil) book.erase(it);
    }
}

void MapOrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
    const std::uint32_t slot = alloc_slot();
    Level& level = side == Side::Buy ? bids_[price] : asks_[price];

    pool_[slot] = Order{id, price, qty, side, level.tail, kNil};
    if (level.tail != kNil)
        pool_[level.tail].next = slot;
    else
        level.head = slot;
    level.tail = slot;
    level.total += qty;

    index_.emplace(id, slot);
}

void MapOrderBook::unlink(std::uint32_t slot) {
    Order& o = pool_[slot];
    auto unlink_from = [&](auto& book) {
        auto it = book.find(o.price);
        Level& level = it->second;
        if (o.prev != kNil)
            pool_[o.prev].next = o.next;
        else
            level.head = o.next;
        if (o.next != kNil)
            pool_[o.next].prev = o.prev;
        else
            level.tail = o.prev;
        level.total -= o.qty;
        if (level.head == kNil) book.erase(it);
    };
    if (o.side == Side::Buy)
        unlink_from(bids_);
    else
        unlink_from(asks_);
}

Result MapOrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return Result::UnknownId;
    const std::uint32_t slot = it->second;
    unlink(slot);
    index_.erase(it);
    free_slot(slot);
    return Result::Ok;
}

Result MapOrderBook::modify(OrderId id, Price new_price, Qty new_qty, std::vector<Trade>& trades) {
    auto it = index_.find(id);
    if (it == index_.end()) return Result::UnknownId;
    if (new_qty == 0) return cancel(id);

    Order& o = pool_[it->second];
    if (new_price == o.price && new_qty <= o.qty) {
        // Pure size reduction: stay in place, keep priority.
        const Qty delta = o.qty - new_qty;
        o.qty = new_qty;
        if (o.side == Side::Buy)
            bids_.find(o.price)->second.total -= delta;
        else
            asks_.find(o.price)->second.total -= delta;
        return Result::Ok;
    }

    const Side side = o.side;
    cancel(id);
    return add_limit(id, side, new_price, new_qty, trades);
}

std::optional<Price> MapOrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> MapOrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Qty MapOrderBook::depth_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total;
}

std::size_t MapOrderBook::level_count(Side side) const { return side == Side::Buy ? bids_.size() : asks_.size(); }

}  // namespace lob
