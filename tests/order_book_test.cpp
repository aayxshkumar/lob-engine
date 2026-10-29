#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

#include "reference_book.hpp"

using namespace lob;

namespace {

// Every test runs against both implementations: the current OrderBook and the
// v0 MapOrderBook baseline.
using Books = ::testing::Types<OrderBook, MapOrderBook>;

template <typename B>
struct BookTest : ::testing::Test {
    B book{1024};
    std::vector<Trade> trades;
};
TYPED_TEST_SUITE(BookTest, Books);

template <typename B>
struct BookProperty : ::testing::Test {};
TYPED_TEST_SUITE(BookProperty, Books);

}  // namespace

TYPED_TEST(BookTest, EmptyBookHasNoPrices) {
    EXPECT_FALSE(this->book.best_bid());
    EXPECT_FALSE(this->book.best_ask());
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(BookTest, NonCrossingOrdersRest) {
    ASSERT_EQ(this->book.add_limit(1, Side::Buy, 100, 10, this->trades), Result::Ok);
    ASSERT_EQ(this->book.add_limit(2, Side::Sell, 101, 5, this->trades), Result::Ok);
    EXPECT_TRUE(this->trades.empty());
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.best_ask(), 101);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 100), 10u);
    EXPECT_EQ(this->book.depth_at(Side::Sell, 101), 5u);
}

TYPED_TEST(BookTest, BestPricesTrackTopOfBook) {
    this->book.add_limit(1, Side::Buy, 99, 1, this->trades);
    this->book.add_limit(2, Side::Buy, 100, 1, this->trades);
    this->book.add_limit(3, Side::Buy, 98, 1, this->trades);
    this->book.add_limit(4, Side::Sell, 103, 1, this->trades);
    this->book.add_limit(5, Side::Sell, 102, 1, this->trades);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.best_ask(), 102);
}

TYPED_TEST(BookTest, CrossingOrderTradesAtMakerPrice) {
    this->book.add_limit(1, Side::Sell, 101, 5, this->trades);
    this->book.add_limit(2, Side::Buy, 105, 5, this->trades);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);
    EXPECT_EQ(this->trades[0].taker_id, 2u);
    EXPECT_EQ(this->trades[0].price, 101);
    EXPECT_EQ(this->trades[0].qty, 5u);
    EXPECT_FALSE(this->book.best_ask());
    EXPECT_FALSE(this->book.best_bid());  // fully filled, nothing rests
}

TYPED_TEST(BookTest, TimePriorityWithinLevel) {
    this->book.add_limit(1, Side::Sell, 101, 3, this->trades);
    this->book.add_limit(2, Side::Sell, 101, 3, this->trades);
    this->book.add_limit(3, Side::Sell, 101, 3, this->trades);
    this->book.add_limit(10, Side::Buy, 101, 5, this->trades);
    ASSERT_EQ(this->trades.size(), 2u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);
    EXPECT_EQ(this->trades[0].qty, 3u);
    EXPECT_EQ(this->trades[1].maker_id, 2u);
    EXPECT_EQ(this->trades[1].qty, 2u);
    EXPECT_EQ(this->book.depth_at(Side::Sell, 101), 4u);  // 1 left on #2, 3 on #3
}

TYPED_TEST(BookTest, PricePriorityAcrossLevels) {
    this->book.add_limit(1, Side::Sell, 102, 5, this->trades);
    this->book.add_limit(2, Side::Sell, 101, 5, this->trades);
    this->book.add_limit(10, Side::Buy, 102, 7, this->trades);
    ASSERT_EQ(this->trades.size(), 2u);
    EXPECT_EQ(this->trades[0].maker_id, 2u);  // better price first, despite arriving later
    EXPECT_EQ(this->trades[0].price, 101);
    EXPECT_EQ(this->trades[1].maker_id, 1u);
    EXPECT_EQ(this->trades[1].price, 102);
    EXPECT_EQ(this->trades[1].qty, 2u);
}

TYPED_TEST(BookTest, PartialFillRestsRemainder) {
    this->book.add_limit(1, Side::Sell, 101, 4, this->trades);
    this->book.add_limit(2, Side::Buy, 101, 10, this->trades);
    EXPECT_EQ(this->book.best_bid(), 101);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 101), 6u);
    EXPECT_FALSE(this->book.best_ask());
}

TYPED_TEST(BookTest, LimitStopsAtItsPrice) {
    this->book.add_limit(1, Side::Sell, 101, 5, this->trades);
    this->book.add_limit(2, Side::Sell, 103, 5, this->trades);
    this->book.add_limit(10, Side::Buy, 102, 8, this->trades);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->book.best_bid(), 102);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 102), 3u);
    EXPECT_EQ(this->book.best_ask(), 103);
}

TYPED_TEST(BookTest, SellSideMatchesBids) {
    this->book.add_limit(1, Side::Buy, 100, 5, this->trades);
    this->book.add_limit(2, Side::Buy, 99, 5, this->trades);
    this->book.add_limit(10, Side::Sell, 99, 7, this->trades);
    ASSERT_EQ(this->trades.size(), 2u);
    EXPECT_EQ(this->trades[0].price, 100);
    EXPECT_EQ(this->trades[1].price, 99);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 99), 3u);
}

TYPED_TEST(BookTest, MarketOrderSweepsAndNeverRests) {
    this->book.add_limit(1, Side::Sell, 101, 2, this->trades);
    this->book.add_limit(2, Side::Sell, 102, 2, this->trades);
    ASSERT_EQ(this->book.add_market(10, Side::Buy, 10, this->trades), Result::Ok);
    EXPECT_EQ(this->trades.size(), 2u);
    EXPECT_FALSE(this->book.best_ask());
    EXPECT_FALSE(this->book.best_bid());
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(BookTest, MarketOrderOnEmptyBookDoesNothing) {
    ASSERT_EQ(this->book.add_market(1, Side::Sell, 5, this->trades), Result::Ok);
    EXPECT_TRUE(this->trades.empty());
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(BookTest, CancelRemovesOrderAndEmptyLevel) {
    this->book.add_limit(1, Side::Buy, 100, 5, this->trades);
    this->book.add_limit(2, Side::Buy, 100, 7, this->trades);
    ASSERT_EQ(this->book.cancel(1), Result::Ok);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 100), 7u);
    ASSERT_EQ(this->book.cancel(2), Result::Ok);
    EXPECT_FALSE(this->book.best_bid());
    EXPECT_EQ(this->book.level_count(Side::Buy), 0u);
}

TYPED_TEST(BookTest, CancelMiddleOfQueueKeepsOrder) {
    this->book.add_limit(1, Side::Sell, 101, 1, this->trades);
    this->book.add_limit(2, Side::Sell, 101, 1, this->trades);
    this->book.add_limit(3, Side::Sell, 101, 1, this->trades);
    this->book.cancel(2);
    this->book.add_limit(10, Side::Buy, 101, 2, this->trades);
    ASSERT_EQ(this->trades.size(), 2u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);
    EXPECT_EQ(this->trades[1].maker_id, 3u);
}

TYPED_TEST(BookTest, CancelUnknownOrFilledId) {
    EXPECT_EQ(this->book.cancel(42), Result::UnknownId);
    this->book.add_limit(1, Side::Sell, 101, 1, this->trades);
    this->book.add_limit(2, Side::Buy, 101, 1, this->trades);
    EXPECT_EQ(this->book.cancel(1), Result::UnknownId);  // already filled
}

TYPED_TEST(BookTest, RejectsDuplicateIdAndZeroQty) {
    this->book.add_limit(1, Side::Buy, 100, 5, this->trades);
    EXPECT_EQ(this->book.add_limit(1, Side::Buy, 99, 5, this->trades), Result::DuplicateId);
    EXPECT_EQ(this->book.add_limit(2, Side::Buy, 99, 0, this->trades), Result::InvalidQty);
    EXPECT_EQ(this->book.add_market(1, Side::Sell, 5, this->trades), Result::DuplicateId);
    EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(BookTest, ModifyDownKeepsPriority) {
    this->book.add_limit(1, Side::Sell, 101, 5, this->trades);
    this->book.add_limit(2, Side::Sell, 101, 5, this->trades);
    ASSERT_EQ(this->book.modify(1, 101, 2, this->trades), Result::Ok);
    EXPECT_EQ(this->book.depth_at(Side::Sell, 101), 7u);
    this->book.add_limit(10, Side::Buy, 101, 1, this->trades);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);
}

TYPED_TEST(BookTest, ModifyUpLosesPriority) {
    this->book.add_limit(1, Side::Sell, 101, 5, this->trades);
    this->book.add_limit(2, Side::Sell, 101, 5, this->trades);
    ASSERT_EQ(this->book.modify(1, 101, 8, this->trades), Result::Ok);
    this->book.add_limit(10, Side::Buy, 101, 1, this->trades);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].maker_id, 2u);
}

TYPED_TEST(BookTest, ModifyPriceCanCross) {
    this->book.add_limit(1, Side::Buy, 100, 5, this->trades);
    this->book.add_limit(2, Side::Sell, 102, 3, this->trades);
    ASSERT_EQ(this->book.modify(1, 102, 5, this->trades), Result::Ok);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].maker_id, 2u);
    EXPECT_EQ(this->trades[0].taker_id, 1u);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 102), 2u);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 100), 0u);
}

TYPED_TEST(BookTest, ModifyToZeroCancels) {
    this->book.add_limit(1, Side::Buy, 100, 5, this->trades);
    EXPECT_EQ(this->book.modify(1, 100, 0, this->trades), Result::Ok);
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_EQ(this->book.modify(1, 100, 1, this->trades), Result::UnknownId);
}

TYPED_TEST(BookTest, PoolSlotsAreReused) {
    TypeParam small{2};
    for (OrderId id = 1; id <= 1000; ++id) {
        ASSERT_EQ(small.add_limit(id, Side::Buy, 100, 1, this->trades), Result::Ok);
        ASSERT_EQ(small.cancel(id), Result::Ok);
    }
    EXPECT_EQ(small.order_count(), 0u);
}

// Randomised check: the book must never be crossed after any operation,
// and no trade may have zero quantity.
TYPED_TEST(BookProperty, NeverCrossed) {
    TypeParam book{1 << 16};
    std::vector<Trade> trades;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> op(0, 9), px(90, 110), qty(1, 20);
    std::vector<OrderId> live;
    OrderId next = 1;

    for (int i = 0; i < 50'000; ++i) {
        const int o = op(rng);
        if (o < 6 || live.empty()) {
            const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            book.add_limit(next, s, px(rng), static_cast<Qty>(qty(rng)), trades);
            live.push_back(next++);
        } else if (o < 9) {
            const std::size_t k = rng() % live.size();
            book.cancel(live[k]);  // may already be filled: UnknownId is fine
            live[k] = live.back();
            live.pop_back();
        } else {
            const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            book.add_market(next++, s, static_cast<Qty>(qty(rng)), trades);
        }
        auto bid = book.best_bid();
        auto ask = book.best_ask();
        if (bid && ask) ASSERT_LT(*bid, *ask) << "book crossed at step " << i;
        for (const Trade& t : trades) ASSERT_GT(t.qty, 0u);
        trades.clear();
    }
}

// Differential test: replay the same random flow (adds, crossing orders,
// cancels, modifies, market orders) through OrderBook and the naive
// ReferenceBook, and require identical trades and book state at every step.
TYPED_TEST(BookProperty, MatchesReferenceBook) {
    // 10 seeds (the reference scans linearly, so CI under the sanitizers stays quick).
    // Every step compares trades, best prices, the order count and the depth at
    // every price the flow can reach.
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        TypeParam book{1 << 16};
        test::ReferenceBook ref;
        std::vector<Trade> got, want;
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<int> op(0, 99), px(95, 105), qty(1, 30);
        std::vector<OrderId> ids;
        OrderId next = 1;

        for (int step = 0; step < 20'000; ++step) {
            const int o = op(rng);
            const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            if (o < 55 || ids.empty()) {
                // Mostly passive (buys low, sells high) so the book builds depth
                // and queues form; ~1 in 5 adds crosses.
                const bool cross = op(rng) < 20;
                const Price off = 1 + static_cast<Price>(rng() % 5);
                const Price p = (s == Side::Buy) == cross ? 100 + off : 100 - off;
                const Qty q = static_cast<Qty>(qty(rng));
                book.add_limit(next, s, p, q, got);
                ref.add_limit(next, s, p, q, want);
                ids.push_back(next++);
            } else if (o < 80) {
                const OrderId id = ids[rng() % ids.size()];
                const bool a = book.cancel(id) == Result::Ok;
                const bool b = ref.cancel(id);
                ASSERT_EQ(a, b) << "cancel disagreed at step " << step;
            } else if (o < 92) {
                // Three kinds of modify: shrink in place (keeps priority), grow at
                // the same price (requeues), and move to a random price (requeues,
                // may trade).
                const OrderId id = ids[rng() % ids.size()];
                const auto cur = ref.price_of(id);
                const Qty cur_qty = ref.qty_of(id);
                const int kind = cur ? static_cast<int>(rng() % 3) : 2;
                Price p = px(rng);
                Qty q = static_cast<Qty>(qty(rng));
                if (kind == 0) {
                    p = *cur;
                    q = static_cast<Qty>(1 + rng() % cur_qty);
                } else if (kind == 1) {
                    p = *cur;
                    q = cur_qty + static_cast<Qty>(1 + rng() % 10);
                }
                const bool a = book.modify(id, p, q, got) == Result::Ok;
                const bool b = ref.modify(id, p, q, want);
                ASSERT_EQ(a, b) << "modify disagreed at step " << step;
            } else {
                const Qty q = static_cast<Qty>(qty(rng));
                book.add_market(next, s, q, got);
                ref.add_market(next, s, q, want);
                ++next;
            }

            ASSERT_EQ(got.size(), want.size()) << "trade count differs at step " << step;
            for (std::size_t i = 0; i < got.size(); ++i) {
                ASSERT_EQ(got[i].maker_id, want[i].maker_id) << "step " << step;
                ASSERT_EQ(got[i].taker_id, want[i].taker_id) << "step " << step;
                ASSERT_EQ(got[i].price, want[i].price) << "step " << step;
                ASSERT_EQ(got[i].qty, want[i].qty) << "step " << step;
            }
            ASSERT_EQ(book.best_bid(), ref.best(Side::Buy)) << "step " << step;
            ASSERT_EQ(book.best_ask(), ref.best(Side::Sell)) << "step " << step;
            ASSERT_EQ(book.order_count(), ref.order_count()) << "step " << step;
            for (Price p = 95; p <= 105; ++p) {  // every price the flow can use
                ASSERT_EQ(book.depth_at(Side::Buy, p), ref.depth_at(Side::Buy, p)) << "bid " << p << ", step " << step;
                ASSERT_EQ(book.depth_at(Side::Sell, p), ref.depth_at(Side::Sell, p)) << "ask " << p << ", step " << step;
            }
            got.clear();
            want.clear();
        }
    }
}

// ---- OrderBook (flat array) specifics -------------------------------------

TEST(ArrayBook, RejectsPricesOutsideBand) {
    OrderBook book{16, 100, 200};
    std::vector<Trade> trades;
    EXPECT_EQ(book.add_limit(1, Side::Buy, 99, 1, trades), Result::PriceOutOfRange);
    EXPECT_EQ(book.add_limit(2, Side::Sell, 201, 1, trades), Result::PriceOutOfRange);
    EXPECT_EQ(book.order_count(), 0u);
    ASSERT_EQ(book.add_limit(3, Side::Buy, 150, 1, trades), Result::Ok);
    EXPECT_EQ(book.modify(3, 250, 1, trades), Result::PriceOutOfRange);
    EXPECT_EQ(book.best_bid(), 150);  // a rejected modify leaves the order alone
    EXPECT_EQ(book.depth_at(Side::Buy, 1'000), 0u);
}

TEST(ArrayBook, WorksAtBandEdges) {
    OrderBook book{16, 100, 200};
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 100, 5, trades);
    book.add_limit(2, Side::Sell, 200, 5, trades);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 200);
    book.add_market(3, Side::Sell, 5, trades);
    book.add_market(4, Side::Buy, 5, trades);
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
}

TEST(ArrayBook, FindsNextBestAcrossGapsAfterLevelEmpties) {
    OrderBook book{64};
    std::vector<Trade> trades;
    book.add_limit(1, Side::Buy, 1'000, 1, trades);
    book.add_limit(2, Side::Buy, 900, 1, trades);   // 100-tick gap
    book.add_limit(3, Side::Sell, 1'100, 1, trades);
    book.add_limit(4, Side::Sell, 1'300, 1, trades);
    book.cancel(1);
    EXPECT_EQ(book.best_bid(), 900);
    book.add_market(5, Side::Buy, 1, trades);       // empties the best ask by matching
    EXPECT_EQ(book.best_ask(), 1'300);
    EXPECT_EQ(book.level_count(Side::Buy), 1u);
    EXPECT_EQ(book.level_count(Side::Sell), 1u);
}
