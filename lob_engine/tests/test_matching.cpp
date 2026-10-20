// Core matching semantics: resting, price-time priority, sweeps, crossings.
#include "harness.hpp"

using lob::EventKind;
using lob::Side;

LOB_TEST(resting_limits_update_best_and_volume) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 99.0, 10)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5)), 2u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 101.0, 7)), 3u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 102.0, 3)), 4u);

    EXPECT(book.best_bid().value() == 100.0);
    EXPECT(book.best_ask().value() == 101.0);
    EXPECT_EQ(book.volume_at(Side::Buy, 99.0), 10);
    EXPECT_EQ(book.volume_at(Side::Buy, 100.0), 5);
    EXPECT_EQ(book.volume_at(Side::Sell, 101.0), 7);
    EXPECT_EQ(book.volume_at(Side::Buy, 98.0), 0);
    EXPECT_EQ(book.order_count(), 4u);
    EXPECT_EQ(sink.count(EventKind::Accepted), 4u);
    EXPECT_EQ(sink.count(EventKind::Trade), 0u);
    lobtest::expect_healthy(book);
}

LOB_TEST(price_time_priority_fifo_within_level) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 105.0, 1)), 1u);  // first in FIFO
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 105.0, 2, 2)), 2u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 999.0, 3)), 3u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].order_id, 1u);
    EXPECT_EQ(trades[0].quantity, 1);
    EXPECT_EQ(trades[0].taker_order_id, 3u);
    EXPECT_EQ(trades[1].order_id, 2u);
    EXPECT_EQ(trades[1].quantity, 2);
    EXPECT(!book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    lobtest::expect_healthy(book);
}

LOB_TEST(partial_fill_updates_remaining) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 5)), 1u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 100.0, 2)), 2u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 2);
    EXPECT(trades[0].price == 100.0);
    // maker id 1: filled 2 of 5
    bool saw_maker_fill = false;
    for (const auto &e : sink.events) {
        if (e.kind == EventKind::Filled && e.order_id == 1u) {
            EXPECT_EQ(e.remaining, 3);
            saw_maker_fill = true;
        }
        if (e.kind == EventKind::Filled && e.order_id == 2u) {
            EXPECT_EQ(e.remaining, 0);  // taker fully filled
        }
    }
    EXPECT(saw_maker_fill);
    EXPECT(book.best_ask().value() == 100.0);
    EXPECT_EQ(book.volume_at(Side::Sell, 100.0), 3);
    EXPECT(book.last_trade_price().value() == 100.0);
    lobtest::expect_healthy(book);
}

LOB_TEST(crossing_pure_limit_is_rejected_not_traded) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 5)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 1)), 2u);  // equal price crosses

    EXPECT_EQ(sink.count(EventKind::Rejected), 1u);
    EXPECT(sink.events.back().reason != nullptr &&
           std::string(sink.events.back().reason) == "would-cross-book");
    EXPECT_EQ(sink.count(EventKind::Trade), 0u);
    EXPECT_EQ(book.volume_at(Side::Sell, 100.0), 5);  // book unchanged
    EXPECT_EQ(book.order_count(), 1u);

    // below the ask it rests normally
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 99.5, 4)), 3u);
    EXPECT(book.best_bid().value() == 99.5);
    // sell at/below the bid also rejects
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 99.5, 1)), 4u);
    EXPECT_EQ(sink.count(EventKind::Rejected), 2u);
    // rejections consume an id, so the next submission gets id 5
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 99.6, 1)), 5u);
    EXPECT_EQ(sink.count(EventKind::Accepted), 3u);  // id1 rests, id3 rests, id5 rests
    lobtest::expect_healthy(book);
}

LOB_TEST(marketable_limit_crosses_and_rests_remainder) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 4)), 1u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 101.0, 6)), 2u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 4);
    EXPECT(trades[0].price == 100.0);
    // remainder rests at the limit price, on the buyer's own side
    EXPECT(book.best_bid().value() == 101.0);
    EXPECT_EQ(book.volume_at(Side::Buy, 101.0), 2);
    EXPECT(book.last_trade_price().value() == 100.0);
    EXPECT_EQ(book.order_count(), 1u);
    lobtest::expect_healthy(book);
}

LOB_TEST(market_order_with_empty_book_is_cancelled) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::market(Side::Buy, 5)), 1u);

    EXPECT_EQ(sink.count(EventKind::Accepted), 1u);
    EXPECT_EQ(sink.count(EventKind::Trade), 0u);
    const auto cancelled = sink.of_kind(EventKind::Cancelled);
    EXPECT_EQ(cancelled.size(), 1u);
    EXPECT_EQ(cancelled[0].order_id, 1u);
    EXPECT_EQ(cancelled[0].quantity, 5);  // unfilled remainder
    EXPECT_EQ(book.order_count(), 0u);
    lobtest::expect_healthy(book);
}

LOB_TEST(sweep_matches_each_level_at_maker_price) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 1)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.5, 1)), 2u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 101.0, 1)), 3u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 101.5, 3)), 4u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 3u);
    EXPECT(trades[0].price == 100.0 && trades[0].order_id == 1u);
    EXPECT(trades[1].price == 100.5 && trades[1].order_id == 2u);
    EXPECT(trades[2].price == 101.0 && trades[2].order_id == 3u);
    EXPECT(book.last_trade_price().value() == 101.0);
    EXPECT(!book.best_ask().has_value());
    lobtest::expect_healthy(book);
}

LOB_TEST(fifo_across_smoothed_two_filled_levels) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5, 2)), 2u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Sell, 100.0, 7)), 3u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].order_id, 1u);
    EXPECT_EQ(trades[0].quantity, 5);
    EXPECT_EQ(trades[1].order_id, 2u);
    EXPECT_EQ(trades[1].quantity, 2);
    EXPECT_EQ(book.volume_at(Side::Buy, 100.0), 3);
    lobtest::expect_healthy(book);
}
