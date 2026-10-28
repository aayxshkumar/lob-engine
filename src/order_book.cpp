#include "lob/order_book.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace lob {

OrderBook::OrderBook(std::size_t capacity, Price min_price, Price max_price)
    : min_price_(min_price), max_price_(max_price) {
    if (max_price < min_price) throw std::invalid_argument("max_price < min_price");
    const auto n = static_cast<std::size_t>(max_price - min_price + 1);
    bids_.resize(n);
    asks_.resize(n);
    bid_bits_.assign((n + 63) / 64, 0);
    ask_bits_.assign((n + 63) / 64, 0);
    best_ask_ = static_cast<std::int64_t>(n);

    pool_.resize(capacity);
    free_.reserve(capacity);
    // Hand out low slots first so a fresh book touches memory in order.
    for (std::size_t i = capacity; i-- > 0;) free_.push_back(static_cast<std::uint32_t>(i));
    index_.reserve(capacity);
}

std::uint32_t OrderBook::alloc_slot() {
    if (free_.empty()) throw std::length_error("order pool exhausted");
    std::uint32_t slot = free_.back();
    free_.pop_back();
    return slot;
}

void OrderBook::free_slot(std::uint32_t slot) { free_.push_back(slot); }

Result OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty, std::vector<Trade>& trades) {
    if (qty == 0) return Result::InvalidQty;
    if (!in_range(price)) return Result::PriceOutOfRange;
    if (index_.contains(id)) return Result::DuplicateId;

    Qty remaining = qty;
    match(id, side, price, false, remaining, trades);
    if (remaining > 0) rest(id, side, price, remaining);
    return Result::Ok;
}

Result OrderBook::add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) {
    if (qty == 0) return Result::InvalidQty;
    if (index_.contains(id)) return Result::DuplicateId;

    Qty remaining = qty;
    match(id, side, 0, true, remaining, trades);
    return Result::Ok;
}

void OrderBook::match(OrderId taker, Side taker_side, Price limit, bool is_market, Qty& remaining,
                      std::vector<Trade>& trades) {
    const Side maker_side = opposite(taker_side);
    const auto n = static_cast<std::int64_t>(asks_.size());

    while (remaining > 0) {
        const std::int64_t bi = maker_side == Side::Sell ? best_ask_ : best_bid_;
        if (bi < 0 || bi >= n) break;  // opposite side empty
        const Price level_price = min_price_ + bi;
        if (!is_market) {
            const bool crosses = taker_side == Side::Buy ? level_price <= limit : level_price >= limit;
            if (!crosses) break;
        }

        Level& lvl = level(maker_side, static_cast<std::size_t>(bi));
        while (remaining > 0 && lvl.head != kNil) {
            const std::uint32_t slot = lvl.head;
            Order& maker = pool_[slot];
            const Qty fill = std::min(remaining, maker.qty);

            trades.push_back(Trade{maker.id, taker, level_price, fill});
            remaining -= fill;
            maker.qty -= fill;
            lvl.total -= fill;

            if (maker.qty == 0) {
                lvl.head = maker.next;
                if (lvl.head != kNil)
                    pool_[lvl.head].prev = kNil;
                else
                    lvl.tail = kNil;
                index_.erase(maker.id);
                free_slot(slot);
            }
        }
        if (lvl.head == kNil) level_emptied(maker_side, static_cast<std::size_t>(bi));
    }
}

// Lowest set bit at index >= from, or -1. Callers guarantee one exists
// (the side is non-empty), but the bounds checks keep it safe regardless.
std::int64_t OrderBook::next_set_at_or_above(const std::vector<std::uint64_t>& bits, std::int64_t from) {
    const auto words = static_cast<std::int64_t>(bits.size());
    if (from < 0) from = 0;
    std::int64_t w = from >> 6;
    if (w >= words) return -1;
    std::uint64_t word = bits[static_cast<std::size_t>(w)] & (~0ull << (from & 63));
    while (word == 0) {
        if (++w >= words) return -1;
        word = bits[static_cast<std::size_t>(w)];
    }
    return (w << 6) + std::countr_zero(word);
}

// Highest set bit at index <= from, or -1.
std::int64_t OrderBook::next_set_at_or_below(const std::vector<std::uint64_t>& bits, std::int64_t from) {
    if (from < 0) return -1;
    std::int64_t w = from >> 6;
    const int b = static_cast<int>(from & 63);
    std::uint64_t word = bits[static_cast<std::size_t>(w)] & (b == 63 ? ~0ull : ((1ull << (b + 1)) - 1));
    while (word == 0) {
        if (--w < 0) return -1;
        word = bits[static_cast<std::size_t>(w)];
    }
    return (w << 6) + 63 - std::countl_zero(word);
}

void OrderBook::level_emptied(Side side, std::size_t i) {
    const auto idx = static_cast<std::int64_t>(i);
    // When a side empties completely, jump straight to "none": scanning for a
    // level that doesn't exist would walk the whole price band.
    if (side == Side::Buy) {
        --bid_levels_;
        clear_bit(bid_bits_, i);
        if (bid_levels_ == 0)
            best_bid_ = -1;
        else if (idx == best_bid_)
            best_bid_ = next_set_at_or_below(bid_bits_, idx - 1);
    } else {
        --ask_levels_;
        clear_bit(ask_bits_, i);
        if (ask_levels_ == 0)
            best_ask_ = static_cast<std::int64_t>(asks_.size());
        else if (idx == best_ask_)
        {
            const std::int64_t next = next_set_at_or_above(ask_bits_, idx + 1);
            best_ask_ = next < 0 ? static_cast<std::int64_t>(asks_.size()) : next;
        }
    }
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
    const std::uint32_t slot = alloc_slot();
    const std::size_t i = to_index(price);
    Level& lvl = level(side, i);

    if (lvl.head == kNil) {
        const auto idx = static_cast<std::int64_t>(i);
        if (side == Side::Buy) {
            ++bid_levels_;
            set_bit(bid_bits_, i);
            best_bid_ = std::max(best_bid_, idx);
        } else {
            ++ask_levels_;
            set_bit(ask_bits_, i);
            best_ask_ = std::min(best_ask_, idx);
        }
    }

    pool_[slot] = Order{id, price, qty, side, lvl.tail, kNil};
    if (lvl.tail != kNil)
        pool_[lvl.tail].next = slot;
    else
        lvl.head = slot;
    lvl.tail = slot;
    lvl.total += qty;

    index_.emplace(id, slot);
}

void OrderBook::unlink(std::uint32_t slot) {
    Order& o = pool_[slot];
    const std::size_t i = to_index(o.price);
    Level& lvl = level(o.side, i);
    if (o.prev != kNil)
        pool_[o.prev].next = o.next;
    else
        lvl.head = o.next;
    if (o.next != kNil)
        pool_[o.next].prev = o.prev;
    else
        lvl.tail = o.prev;
    lvl.total -= o.qty;
    if (lvl.head == kNil) level_emptied(o.side, i);
}

Result OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return Result::UnknownId;
    const std::uint32_t slot = it->second;
    unlink(slot);
    index_.erase(it);
    free_slot(slot);
    return Result::Ok;
}

Result OrderBook::modify(OrderId id, Price new_price, Qty new_qty, std::vector<Trade>& trades) {
    auto it = index_.find(id);
    if (it == index_.end()) return Result::UnknownId;
    if (new_qty == 0) return cancel(id);
    if (!in_range(new_price)) return Result::PriceOutOfRange;

    Order& o = pool_[it->second];
    if (new_price == o.price && new_qty <= o.qty) {
        // Pure size reduction: stay in place, keep priority.
        level(o.side, to_index(o.price)).total -= o.qty - new_qty;
        o.qty = new_qty;
        return Result::Ok;
    }

    const Side side = o.side;
    cancel(id);
    return add_limit(id, side, new_price, new_qty, trades);
}

std::optional<Price> OrderBook::best_bid() const {
    if (best_bid_ < 0) return std::nullopt;
    return min_price_ + best_bid_;
}

std::optional<Price> OrderBook::best_ask() const {
    if (best_ask_ >= static_cast<std::int64_t>(asks_.size())) return std::nullopt;
    return min_price_ + best_ask_;
}

Qty OrderBook::depth_at(Side side, Price price) const {
    if (!in_range(price)) return 0;
    const std::size_t i = to_index(price);
    return side == Side::Buy ? bids_[i].total : asks_[i].total;
}

}  // namespace lob
