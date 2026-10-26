#include <gtest/gtest.h>

#include <vector>

#include "lob/book.hpp"
#include "lob/engine.hpp"

namespace lob {

bool operator==(const Fill& a, const Fill& b) {
    return a.maker_id == b.maker_id && a.taker_id == b.taker_id && a.price == b.price && a.qty == b.qty;
}
void PrintTo(const Fill& f, std::ostream* os) {
    *os << "{maker " << f.maker_id << ", taker " << f.taker_id << ", " << f.qty << " @ " << f.price << "}";
}

namespace {

struct Recorder {
    std::vector<Fill> fills;
    void on_fill(const Fill& f) noexcept { fills.push_back(f); }
};

class BookTest : public ::testing::Test {
protected:
    static constexpr BookConfig kConfig{1000, 256, 64};
    Recorder rec;
    Book<Recorder> book{kConfig, rec};

    void expect_top(TopOfBook top, Price price, std::uint64_t qty) {
        EXPECT_EQ(top.qty, qty);
        if (qty) EXPECT_EQ(top.price, price);
    }
};

TEST_F(BookTest, EmptyBook) {
    expect_top(book.best_bid(), 0, 0);
    expect_top(book.best_ask(), 0, 0);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.cancel(1), Result::UnknownOrderId);
    EXPECT_EQ(book.modify(1, 5), Result::UnknownOrderId);
    EXPECT_EQ(book.add_market(1, Side::Buy, 10), Result::Ok);
    EXPECT_TRUE(rec.fills.empty());
    EXPECT_FALSE(book.contains(1));
}

TEST_F(BookTest, AddRestsAndAggregatesLevel) {
    EXPECT_EQ(book.add_limit(1, Side::Buy, 1010, 5), Result::Ok);
    EXPECT_EQ(book.add_limit(2, Side::Buy, 1010, 7), Result::Ok);
    EXPECT_EQ(book.add_limit(3, Side::Buy, 1005, 1), Result::Ok);
    EXPECT_EQ(book.add_limit(4, Side::Sell, 1020, 3), Result::Ok);
    expect_top(book.best_bid(), 1010, 12);
    expect_top(book.best_ask(), 1020, 3);
    EXPECT_EQ(book.depth(Side::Buy, 1005), 1u);
    EXPECT_EQ(book.order_count(), 4u);
    EXPECT_TRUE(rec.fills.empty());
}

TEST_F(BookTest, RejectsInvalidInput) {
    EXPECT_EQ(book.add_limit(1, Side::Buy, 999, 1), Result::InvalidPrice);
    EXPECT_EQ(book.add_limit(1, Side::Buy, 1256, 1), Result::InvalidPrice);
    EXPECT_EQ(book.add_limit(1, Side::Buy, 1000, 0), Result::InvalidQuantity);
    EXPECT_EQ(book.add_limit(1, static_cast<Side>('Z'), 1000, 1), Result::InvalidSide);
    EXPECT_EQ(book.add_limit(1, Side::Buy, 1000, 1), Result::Ok);
    EXPECT_EQ(book.add_limit(1, Side::Sell, 1100, 1), Result::DuplicateOrderId);
    EXPECT_EQ(book.add_market(1, Side::Sell, 1), Result::DuplicateOrderId);
    EXPECT_EQ(book.add_limit(2, Side::Buy, 1255, 1), Result::Ok);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST_F(BookTest, CancelRemovesAndUpdatesBest) {
    book.add_limit(1, Side::Buy, 1010, 5);
    book.add_limit(2, Side::Buy, 1008, 5);
    book.add_limit(3, Side::Buy, 1010, 2);
    EXPECT_EQ(book.cancel(1), Result::Ok);
    expect_top(book.best_bid(), 1010, 2);
    EXPECT_EQ(book.cancel(3), Result::Ok);
    expect_top(book.best_bid(), 1008, 5);
    EXPECT_EQ(book.cancel(3), Result::UnknownOrderId);
    EXPECT_EQ(book.cancel(2), Result::Ok);
    expect_top(book.best_bid(), 0, 0);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(BookTest, CancelMiddleOfQueueKeepsFifo) {
    book.add_limit(1, Side::Sell, 1050, 1);
    book.add_limit(2, Side::Sell, 1050, 1);
    book.add_limit(3, Side::Sell, 1050, 1);
    book.cancel(2);
    book.add_limit(10, Side::Buy, 1050, 2);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 10, 1050, 1}, {3, 10, 1050, 1}}));
}

TEST_F(BookTest, ModifyReducesInPlaceKeepingPriority) {
    book.add_limit(1, Side::Buy, 1010, 10);
    book.add_limit(2, Side::Buy, 1010, 10);
    EXPECT_EQ(book.modify(1, 4), Result::Ok);
    expect_top(book.best_bid(), 1010, 14);
    EXPECT_EQ(book.modify(1, 4), Result::InvalidQuantity);
    EXPECT_EQ(book.modify(1, 5), Result::InvalidQuantity);
    EXPECT_EQ(book.modify(1, 0), Result::InvalidQuantity);
    book.add_limit(3, Side::Sell, 1010, 5);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 3, 1010, 4}, {2, 3, 1010, 1}}));
}

TEST_F(BookTest, PartialFillOfMakerLeavesRemainder) {
    book.add_limit(1, Side::Sell, 1020, 10);
    EXPECT_EQ(book.add_limit(2, Side::Buy, 1020, 4), Result::Ok);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 2, 1020, 4}}));
    expect_top(book.best_ask(), 1020, 6);
    expect_top(book.best_bid(), 0, 0);
    EXPECT_FALSE(book.contains(2));
}

TEST_F(BookTest, PartialFillOfTakerRestsRemainder) {
    book.add_limit(1, Side::Sell, 1020, 3);
    book.add_limit(2, Side::Buy, 1025, 10);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 2, 1020, 3}}));
    expect_top(book.best_bid(), 1025, 7);
    expect_top(book.best_ask(), 0, 0);
    EXPECT_FALSE(book.contains(1));
}

TEST_F(BookTest, FullFillBothSides) {
    book.add_limit(1, Side::Buy, 1010, 5);
    book.add_limit(2, Side::Sell, 1010, 5);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 2, 1010, 5}}));
    EXPECT_EQ(book.order_count(), 0u);
    expect_top(book.best_bid(), 0, 0);
    expect_top(book.best_ask(), 0, 0);
}

TEST_F(BookTest, PriceTimePriorityAcrossLevels) {
    book.add_limit(1, Side::Sell, 1030, 5);
    book.add_limit(2, Side::Sell, 1020, 5);
    book.add_limit(3, Side::Sell, 1020, 5);
    book.add_limit(4, Side::Sell, 1025, 5);
    book.add_limit(9, Side::Buy, 1030, 18);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{2, 9, 1020, 5}, {3, 9, 1020, 5}, {4, 9, 1025, 5}, {1, 9, 1030, 3}}));
    expect_top(book.best_ask(), 1030, 2);
}

TEST_F(BookTest, CrossingTradesAtMakerPrice) {
    book.add_limit(1, Side::Buy, 1050, 5);
    book.add_limit(2, Side::Sell, 1001, 2);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 2, 1050, 2}}));
    expect_top(book.best_bid(), 1050, 3);
}

TEST_F(BookTest, LimitStopsAtItsPrice) {
    book.add_limit(1, Side::Buy, 1040, 5);
    book.add_limit(2, Side::Buy, 1030, 5);
    book.add_limit(3, Side::Sell, 1035, 20);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 3, 1040, 5}}));
    expect_top(book.best_bid(), 1030, 5);
    expect_top(book.best_ask(), 1035, 15);
}

TEST_F(BookTest, MarketOrderSweepsAndNeverRests) {
    book.add_limit(1, Side::Buy, 1040, 5);
    book.add_limit(2, Side::Buy, 1000, 5);
    EXPECT_EQ(book.add_market(3, Side::Sell, 20), Result::Ok);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 3, 1040, 5}, {2, 3, 1000, 5}}));
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.contains(3));
    expect_top(book.best_ask(), 0, 0);
}

TEST_F(BookTest, ExtremeLevels) {
    book.add_limit(1, Side::Sell, 1255, 1);
    book.add_limit(2, Side::Buy, 1000, 1);
    expect_top(book.best_ask(), 1255, 1);
    expect_top(book.best_bid(), 1000, 1);
    book.add_limit(3, Side::Buy, 1255, 2);
    expect_top(book.best_bid(), 1255, 1);
    book.add_limit(4, Side::Sell, 1000, 3);
    EXPECT_EQ(rec.fills, (std::vector<Fill>{{1, 3, 1255, 1}, {3, 4, 1255, 1}, {2, 4, 1000, 1}}));
    expect_top(book.best_bid(), 0, 0);
    expect_top(book.best_ask(), 1000, 1);
}

TEST_F(BookTest, PoolExhaustionRejectsWithoutSideEffects) {
    for (OrderId id = 1; id <= kConfig.max_orders; ++id) ASSERT_EQ(book.add_limit(id, Side::Buy, 1000, 1), Result::Ok);
    EXPECT_EQ(book.add_limit(999, Side::Buy, 1001, 1), Result::PoolExhausted);
    EXPECT_EQ(book.add_limit(999, Side::Sell, 1000, 1), Result::PoolExhausted);
    EXPECT_TRUE(rec.fills.empty());
    expect_top(book.best_bid(), 1000, kConfig.max_orders);
    // Market orders never need a node.
    EXPECT_EQ(book.add_market(999, Side::Sell, 1), Result::Ok);
    EXPECT_EQ(book.add_limit(1000, Side::Buy, 1001, 1), Result::Ok);
    EXPECT_EQ(book.cancel(1000), Result::Ok);
    EXPECT_EQ(book.add_limit(1001, Side::Buy, 1001, 1), Result::Ok);
}

TEST_F(BookTest, ClearResetsEverything) {
    for (OrderId id = 1; id <= 40; ++id)
        book.add_limit(id, id % 2 ? Side::Buy : Side::Sell, id % 2 ? 1000 + Price(id) : 1100 + Price(id), 3);
    book.clear();
    EXPECT_EQ(book.order_count(), 0u);
    expect_top(book.best_bid(), 0, 0);
    expect_top(book.best_ask(), 0, 0);
    EXPECT_EQ(book.depth(Side::Buy, 1001), 0u);
    for (OrderId id = 1; id <= kConfig.max_orders; ++id) ASSERT_EQ(book.add_limit(id, Side::Sell, 1200, 1), Result::Ok);
}

TEST(EngineTest, PublishesFillsThenAck) {
    InboundQueue in(64);
    OutboundQueue out(64);
    Engine engine({0, 128, 16}, in, out);
    in.push(make_add_limit(1, Side::Sell, 50, 5));
    InboundMsg buy = make_add_limit(2, Side::Buy, 60, 3);
    buy.timestamp = 77;
    in.push(buy);
    in.push(make_cancel(42));
    while (engine.poll()) {
    }
    std::vector<OutboundMsg> msgs;
    while (const OutboundMsg* m = out.front()) {
        msgs.push_back(*m);
        out.pop();
    }
    ASSERT_EQ(msgs.size(), 4u);
    EXPECT_EQ(msgs[0].type, OutType::Ack);
    EXPECT_EQ(msgs[1].type, OutType::Fill);
    EXPECT_EQ(msgs[1].maker_id, 1u);
    EXPECT_EQ(msgs[1].id, 2u);
    EXPECT_EQ(msgs[1].price, 50);
    EXPECT_EQ(msgs[1].qty, 3u);
    EXPECT_EQ(msgs[1].timestamp, 77u);
    EXPECT_EQ(msgs[2].type, OutType::Ack);
    EXPECT_EQ(msgs[2].timestamp, 77u);
    EXPECT_EQ(msgs[3].result, Result::UnknownOrderId);
    EXPECT_EQ(engine.book().best_ask().qty, 2u);
}

}  // namespace
}  // namespace lob
