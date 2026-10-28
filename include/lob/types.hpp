#pragma once

#include <cstdint>

namespace lob {

// Prices are integer ticks, never floating point: exchanges quote on a fixed
// tick grid, and integer comparison is exact and cheap.
using Price = std::int64_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

struct Trade {
    OrderId maker_id;  // resting order
    OrderId taker_id;  // incoming order
    Price price;       // always the maker's price
    Qty qty;
};

enum class Result : std::uint8_t {
    Ok,
    DuplicateId,
    UnknownId,
    InvalidQty,
    PriceOutOfRange,
};

}  // namespace lob
