// Dormant stop orders: trigger detection, conversion, remainder handling,
// cascades, cancel/modify while dormant.
#include "harness.hpp"

using lob::EventKind;
using lob::Side;

LOB_TEST(dormant_stop_does_not_touch_the_book) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 101.0, 5)), 1u);
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 105.0, 4)), 2u);  // no last price yet

    EXPECT(!book.best_bid().has_value());  // stop is not in the resting book
    EXPECT_EQ(book.stop_volume_at(Side::Buy, 105.0), 4);
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(sink.count(EventKind::Triggered), 0u);
    lobtest::expect_healthy(book);
}

LOB_TEST(buy_stop_triggers_when_last_reaches_trigger) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 5)), 1u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 100.0, 1)), 2u);
    EXPECT(book.last_trade_price().value() == 100.0);

    // last (100) already met trigger: activates immediately on submit;
    // sweeps the remaining 4 @100 (order 1 fully filled, 1 - 1... the ask
    // level holds 5 total, 1 was taken, so 4 remain)
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 100.0, 3)), 3u);
    const auto triggered = sink.of_kind(EventKind::Triggered);
    EXPECT_EQ(triggered.size(), 1u);
    EXPECT_EQ(triggered[0].order_id, 3u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 2u);  // initial fill + stop conversion fill
    EXPECT_EQ(trades[1].taker_order_id, 3u);
    EXPECT_EQ(trades[1].quantity, 3);
    EXPECT(trades[1].price == 100.0);
    EXPECT_EQ(book.volume_at(Side::Sell, 100.0), 1);  // maker keeps 1
    lobtest::expect_healthy(book);
}

LOB_TEST(sell_stop_triggers_when_last_falls_to_trigger) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 99.0, 5)), 2u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Sell, 99.0, 1)), 3u);  // last = 100

    // trigger (99) above the last price (100) stays dormant
    EXPECT_EQ(book.submit(lobtest::stop(Side::Sell, 99.0, 3)), 4u);
    EXPECT_EQ(sink.count(EventKind::Triggered), 0u);
    EXPECT_EQ(book.stop_volume_at(Side::Sell, 99.0), 3);

    // sweep down through the trigger: bids lose 1 @100, then 2 @99
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Sell, 99.0, 6)), 5u);
    // last becomes 99 → stop fires as a market sell and fills the 3 remaining
    const auto triggered = sink.of_kind(EventKind::Triggered);
    EXPECT_EQ(triggered.size(), 1u);
    EXPECT_EQ(triggered[0].order_id, 4u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 4u);  // id3: 1@100 | id5: 4@100, 2@99 | id4: 3@99
    EXPECT_EQ(trades[3].taker_order_id, 4u);
    EXPECT_EQ(trades[3].quantity, 3);
    EXPECT(trades[3].price == 99.0);
    // fully filled: no cancellation event for the stop
    for (const auto &e : sink.of_kind(EventKind::Cancelled)) {
        EXPECT_EQ(e.order_id != 4u, true);
    }
    EXPECT_EQ(book.stop_volume_at(Side::Sell, 99.0), 0);
    EXPECT(!book.best_bid().has_value());
    lobtest::expect_healthy(book);
}

LOB_TEST(stop_limit_taker_phase_then_rests_remainder) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 2)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 99.5, 5)), 2u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Sell, 99.0, 1)), 3u);  // last = 100

    // last (100) already at/below trigger (100.5): fires on submission.
    // Qty 12 sweeps past both bid levels (1 + 5 used, StopLimit may cross),
    // remainder 6 rests at the limit price on the sell side.
    EXPECT_EQ(book.submit(lobtest::stop_limit(Side::Sell, 100.5, 99.5, 12)), 4u);
    const auto triggered = sink.of_kind(EventKind::Triggered);
    EXPECT_EQ(triggered.size(), 1u);
    EXPECT_EQ(triggered[0].order_id, 4u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 3u);
    EXPECT(trades[0].price == 100.0 && trades[0].quantity == 1);   // order 3's fill
    EXPECT(trades[1].price == 100.0 && trades[1].quantity == 1);
    EXPECT_EQ(trades[1].taker_order_id, 4u);
    EXPECT(trades[2].price == 99.5 && trades[2].quantity == 5);
    EXPECT_EQ(trades[2].taker_order_id, 4u);

    EXPECT(!book.best_bid().has_value());  // bids fully consumed
    EXPECT(book.best_ask().value() == 99.5);
    EXPECT_EQ(book.volume_at(Side::Buy, 99.5), 0);
    EXPECT_EQ(book.volume_at(Side::Sell, 99.5), 6);  // rested remainder
    EXPECT(book.last_trade_price().value() == 99.5);
    EXPECT_EQ(book.order_count(), 1u);
    lobtest::expect_healthy(book);
}

LOB_TEST(stop_cascade_chains_multiple_activations) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 2)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 105.0, 2)), 2u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 110.0, 2)), 3u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 111.0, 2)), 4u);
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 110.0, 2)), 5u);
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 111.0, 2)), 6u);

    // push the last price up to 110: fires stop 5, whose own trade at 111
    // fires stop 6 - both activations from one submission
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 120.0, 6)), 7u);

    const auto triggered = sink.of_kind(EventKind::Triggered);
    EXPECT_EQ(triggered.size(), 2u);
    EXPECT_EQ(triggered[0].order_id, 5u);
    EXPECT_EQ(triggered[1].order_id, 6u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 4u);  // 3 sweep levels + stop 5's conversion trade
    // stop 6 had nothing left to sweep: remainder cancelled
    bool stop6_cancelled = false;
    for (const auto &e : sink.of_kind(EventKind::Cancelled)) {
        if (e.order_id == 6u) {
            EXPECT_EQ(e.quantity, 2);
            stop6_cancelled = true;
        }
    }
    EXPECT(stop6_cancelled);
    EXPECT(book.last_trade_price().value() == 111.0);
    lobtest::expect_healthy(book);
}

LOB_TEST(stops_at_same_trigger_fire_fifo) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 1)), 1u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 100.0, 1)), 2u);  // last = 100

    // two dormant stops at the same trigger: FIFO activation order
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 102.0, 1)), 3u);
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 102.0, 1, 4)), 4u);
    EXPECT_EQ(book.stop_volume_at(Side::Buy, 102.0), 2);

    EXPECT(book.cancel(3u));
    EXPECT_EQ(book.stop_volume_at(Side::Buy, 102.0), 1);

    // move the surviving stop to a nearer trigger
    lob::ModifyRequest req;
    req.has_new_stop_price = true;
    req.new_stop_price = 101.0;
    EXPECT(book.modify(4u, req));
    EXPECT_EQ(book.stop_volume_at(Side::Buy, 102.0), 0);
    EXPECT_EQ(book.stop_volume_at(Side::Buy, 101.0), 1);

    // print at 101: the stop converts, but the ask is already gone →
    // its unfilled remainder is cancelled
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 101.0, 1, 5)), 5u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 101.0, 1)), 6u);

    const auto triggered = sink.of_kind(EventKind::Triggered);
    EXPECT_EQ(triggered.size(), 1u);
    EXPECT_EQ(triggered[0].order_id, 4u);
    bool stop4_cancelled = false;
    for (const auto &e : sink.of_kind(EventKind::Cancelled)) {
        if (e.order_id == 4u && e.quantity == 1) stop4_cancelled = true;
    }
    EXPECT(stop4_cancelled);
    lobtest::expect_healthy(book);
}

LOB_TEST(stop_of_both_sides_rest_before_any_trade) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 110.0, 2)), 1u);
    EXPECT_EQ(book.submit(lobtest::stop(Side::Sell, 90.0, 3)), 2u);
    EXPECT_EQ(book.stop_volume_at(Side::Buy, 110.0), 2);
    EXPECT_EQ(book.stop_volume_at(Side::Sell, 90.0), 3);
    EXPECT_EQ(sink.count(EventKind::Triggered), 0u);

    // a marketable trade prints a price above 110? not possible: book empty.
    // seed the book, print at 110 to fire the buy stop, then down at 90.
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 110.0, 1)), 3u);
    EXPECT_EQ(book.submit(lobtest::market(Side::Buy, 1)), 4u);   // last = 110
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 90.0, 1)), 5u);
    EXPECT_EQ(book.submit(lobtest::market(Side::Sell, 1)), 6u);  // last = 90

    const auto triggered = sink.of_kind(EventKind::Triggered);
    EXPECT_EQ(triggered.size(), 2u);
    EXPECT_EQ(triggered[0].order_id, 1u);
    EXPECT_EQ(triggered[1].order_id, 2u);
    lobtest::expect_healthy(book);
}

LOB_TEST(stop_trigger_order_is_price_then_time) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    // last price starts at 95 (buy stops strictly above that stay dormant)
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 95.0, 1)), 1u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 95.0, 1)), 2u);

    // submitted out of trigger order: 96 lower than 97 → fires first
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 97.0, 1, 3)), 3u);
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 96.0, 1, 4)), 4u);
    EXPECT_EQ(sink.count(EventKind::Triggered), 0u);

    // last rises to 96 → stop 4 fires; rises to 97 → stop 3 fires
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 96.0, 1, 5)), 5u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 96.0, 1)), 6u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 97.0, 1, 7)), 7u);
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 97.0, 1)), 8u);

    const auto triggered = sink.of_kind(EventKind::Triggered);
    EXPECT_EQ(triggered.size(), 2u);
    EXPECT_EQ(triggered[0].order_id, 4u);  // 96 first
    EXPECT_EQ(triggered[1].order_id, 3u);  // then 97
    lobtest::expect_healthy(book);
}
