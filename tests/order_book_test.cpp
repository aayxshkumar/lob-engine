#include "lob/order_book.hpp"

#include <gtest/gtest.h>

#include <random>

#include "reference_book.hpp"

using namespace lob;

namespace {

struct BookTest : ::testing::Test {
    OrderBook book{1024};
    std::vector<Trade> trades;
};

}  // namespace

TEST_F(BookTest, EmptyBookHasNoPrices) {
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, NonCrossingOrdersRest) {
    ASSERT_EQ(book.add_limit(1, Side::Buy, 100, 10, trades), Result::Ok);
    ASSERT_EQ(book.add_limit(2, Side::Sell, 101, 5, trades), Result::Ok);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.depth_at(Side::Buy, 100), 10u);
    EXPECT_EQ(book.depth_at(Side::Sell, 101), 5u);
}

TEST_F(BookTest, BestPricesTrackTopOfBook) {
    book.add_limit(1, Side::Buy, 99, 1, trades);
    book.add_limit(2, Side::Buy, 100, 1, trades);
    book.add_limit(3, Side::Buy, 98, 1, trades);
    book.add_limit(4, Side::Sell, 103, 1, trades);
    book.add_limit(5, Side::Sell, 102, 1, trades);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 102);
}

TEST_F(BookTest, CrossingOrderTradesAtMakerPrice) {
    book.add_limit(1, Side::Sell, 101, 5, trades);
    book.add_limit(2, Side::Buy, 105, 5, trades);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[0].taker_id, 2u);
    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[0].qty, 5u);
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.best_bid());  // fully filled, nothing rests
}

TEST_F(BookTest, TimePriorityWithinLevel) {
    book.add_limit(1, Side::Sell, 101, 3, trades);
    book.add_limit(2, Side::Sell, 101, 3, trades);
    book.add_limit(3, Side::Sell, 101, 3, trades);
    book.add_limit(10, Side::Buy, 101, 5, trades);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[0].qty, 3u);
    EXPECT_EQ(trades[1].maker_id, 2u);
    EXPECT_EQ(trades[1].qty, 2u);
    EXPECT_EQ(book.depth_at(Side::Sell, 101), 4u);  // 1 left on #2, 3 on #3
}

TEST_F(BookTest, PricePriorityAcrossLevels) {
    book.add_limit(1, Side::Sell, 102, 5, trades);
    book.add_limit(2, Side::Sell, 101, 5, trades);
    book.add_limit(10, Side::Buy, 102, 7, trades);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_id, 2u);  // better price first, despite arriving later
    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[1].maker_id, 1u);
    EXPECT_EQ(trades[1].price, 102);
    EXPECT_EQ(trades[1].qty, 2u);
}

TEST_F(BookTest, PartialFillRestsRemainder) {
    book.add_limit(1, Side::Sell, 101, 4, trades);
    book.add_limit(2, Side::Buy, 101, 10, trades);
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.depth_at(Side::Buy, 101), 6u);
    EXPECT_FALSE(book.best_ask());
}

TEST_F(BookTest, LimitStopsAtItsPrice) {
    book.add_limit(1, Side::Sell, 101, 5, trades);
    book.add_limit(2, Side::Sell, 103, 5, trades);
    book.add_limit(10, Side::Buy, 102, 8, trades);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(book.best_bid(), 102);
    EXPECT_EQ(book.depth_at(Side::Buy, 102), 3u);
    EXPECT_EQ(book.best_ask(), 103);
}

TEST_F(BookTest, SellSideMatchesBids) {
    book.add_limit(1, Side::Buy, 100, 5, trades);
    book.add_limit(2, Side::Buy, 99, 5, trades);
    book.add_limit(10, Side::Sell, 99, 7, trades);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 99);
    EXPECT_EQ(book.depth_at(Side::Buy, 99), 3u);
}

TEST_F(BookTest, MarketOrderSweepsAndNeverRests) {
    book.add_limit(1, Side::Sell, 101, 2, trades);
    book.add_limit(2, Side::Sell, 102, 2, trades);
    ASSERT_EQ(book.add_market(10, Side::Buy, 10, trades), Result::Ok);
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.best_bid());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, MarketOrderOnEmptyBookDoesNothing) {
    ASSERT_EQ(book.add_market(1, Side::Sell, 5, trades), Result::Ok);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, CancelRemovesOrderAndEmptyLevel) {
    book.add_limit(1, Side::Buy, 100, 5, trades);
    book.add_limit(2, Side::Buy, 100, 7, trades);
    ASSERT_EQ(book.cancel(1), Result::Ok);
    EXPECT_EQ(book.depth_at(Side::Buy, 100), 7u);
    ASSERT_EQ(book.cancel(2), Result::Ok);
    EXPECT_FALSE(book.best_bid());
    EXPECT_EQ(book.level_count(Side::Buy), 0u);
}

TEST_F(BookTest, CancelMiddleOfQueueKeepsOrder) {
    book.add_limit(1, Side::Sell, 101, 1, trades);
    book.add_limit(2, Side::Sell, 101, 1, trades);
    book.add_limit(3, Side::Sell, 101, 1, trades);
    book.cancel(2);
    book.add_limit(10, Side::Buy, 101, 2, trades);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[1].maker_id, 3u);
}

TEST_F(BookTest, CancelUnknownOrFilledId) {
    EXPECT_EQ(book.cancel(42), Result::UnknownId);
    book.add_limit(1, Side::Sell, 101, 1, trades);
    book.add_limit(2, Side::Buy, 101, 1, trades);
    EXPECT_EQ(book.cancel(1), Result::UnknownId);  // already filled
}

TEST_F(BookTest, RejectsDuplicateIdAndZeroQty) {
    book.add_limit(1, Side::Buy, 100, 5, trades);
    EXPECT_EQ(book.add_limit(1, Side::Buy, 99, 5, trades), Result::DuplicateId);
    EXPECT_EQ(book.add_limit(2, Side::Buy, 99, 0, trades), Result::InvalidQty);
    EXPECT_EQ(book.add_market(1, Side::Sell, 5, trades), Result::DuplicateId);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, ModifyDownKeepsPriority) {
    book.add_limit(1, Side::Sell, 101, 5, trades);
    book.add_limit(2, Side::Sell, 101, 5, trades);
    ASSERT_EQ(book.modify(1, 101, 2, trades), Result::Ok);
    EXPECT_EQ(book.depth_at(Side::Sell, 101), 7u);
    book.add_limit(10, Side::Buy, 101, 1, trades);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 1u);
}

TEST_F(BookTest, ModifyUpLosesPriority) {
    book.add_limit(1, Side::Sell, 101, 5, trades);
    book.add_limit(2, Side::Sell, 101, 5, trades);
    ASSERT_EQ(book.modify(1, 101, 8, trades), Result::Ok);
    book.add_limit(10, Side::Buy, 101, 1, trades);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
}

TEST_F(BookTest, ModifyPriceCanCross) {
    book.add_limit(1, Side::Buy, 100, 5, trades);
    book.add_limit(2, Side::Sell, 102, 3, trades);
    ASSERT_EQ(book.modify(1, 102, 5, trades), Result::Ok);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
    EXPECT_EQ(trades[0].taker_id, 1u);
    EXPECT_EQ(book.depth_at(Side::Buy, 102), 2u);
    EXPECT_EQ(book.depth_at(Side::Buy, 100), 0u);
}

TEST_F(BookTest, ModifyToZeroCancels) {
    book.add_limit(1, Side::Buy, 100, 5, trades);
    EXPECT_EQ(book.modify(1, 100, 0, trades), Result::Ok);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.modify(1, 100, 1, trades), Result::UnknownId);
}

TEST_F(BookTest, PoolSlotsAreReused) {
    OrderBook small{2};
    for (OrderId id = 1; id <= 1000; ++id) {
        ASSERT_EQ(small.add_limit(id, Side::Buy, 100, 1, trades), Result::Ok);
        ASSERT_EQ(small.cancel(id), Result::Ok);
    }
    EXPECT_EQ(small.order_count(), 0u);
}

// Randomised check: the book must never be crossed after any operation,
// and no trade may have zero quantity.
TEST(OrderBookFuzz, NeverCrossed) {
    OrderBook book{1 << 16};
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
TEST(OrderBookDifferential, MatchesReferenceBook) {
    OrderBook book{1 << 16};
    test::ReferenceBook ref;
    std::vector<Trade> got, want;
    std::mt19937_64 rng(1234);
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
        got.clear();
        want.clear();
    }
    for (Price p = 95; p <= 105; ++p) {
        EXPECT_EQ(book.depth_at(Side::Buy, p), ref.depth_at(Side::Buy, p));
        EXPECT_EQ(book.depth_at(Side::Sell, p), ref.depth_at(Side::Sell, p));
    }
}
