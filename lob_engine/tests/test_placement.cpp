// Placement operations: cancel, modify, validation, id management.
#include "harness.hpp"

#include <string>

using lob::EventKind;
using lob::Side;

LOB_TEST(cancel_removes_order_and_rejects_twice) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 99.0, 4)), 2u);

    EXPECT(book.cancel(2u));
    EXPECT(book.best_bid().value() == 100.0);
    EXPECT_EQ(book.volume_at(Side::Buy, 99.0), 0);
    EXPECT_EQ(book.order_count(), 1u);

    EXPECT(!book.cancel(2u));  // already gone
    const auto rejected = sink.of_kind(EventKind::Rejected);
    EXPECT_EQ(rejected.size(), 1u);
    EXPECT(rejected[0].reason != nullptr && std::string(rejected[0].reason) == "unknown-order");

    const auto cancelled = sink.of_kind(EventKind::Cancelled);
    EXPECT_EQ(cancelled.size(), 1u);
    EXPECT_EQ(cancelled[0].order_id, 2u);
    EXPECT_EQ(cancelled[0].quantity, 4);
    lobtest::expect_healthy(book);
}

LOB_TEST(cancel_from_middle_keeps_fifo_of_others) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 2, 1)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 3, 2)), 2u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 4, 3)), 3u);
    EXPECT(book.cancel(2u));
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 100.0, 6)), 4u);

    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].order_id, 1u);
    EXPECT_EQ(trades[0].quantity, 2);
    EXPECT_EQ(trades[1].order_id, 3u);
    EXPECT_EQ(trades[1].quantity, 4);
    lobtest::expect_healthy(book);
}

LOB_TEST(modify_reduce_keeps_priority) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 5, 1)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 5, 2)), 2u);

    lob::ModifyRequest req;
    req.has_new_quantity = true;
    req.new_quantity = 2;
    EXPECT(book.modify(1u, req));
    EXPECT_EQ(sink.count(EventKind::Modified), 1u);

    // reduced order keeps its slot: full sweep hits id 1 first
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 999.0, 7)), 3u);
    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].order_id, 1u);
    EXPECT_EQ(trades[0].quantity, 2);
    EXPECT_EQ(trades[1].order_id, 2u);
    EXPECT_EQ(trades[1].quantity, 5);
    lobtest::expect_healthy(book);
}

LOB_TEST(modify_increase_loses_priority) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 5, 1)), 1u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 100.0, 5, 2)), 2u);

    lob::ModifyRequest req;
    req.has_new_quantity = true;
    req.new_quantity = 10;
    EXPECT(book.modify(1u, req));

    // top-up pushes the order behind everyone at its price
    EXPECT_EQ(book.submit(lobtest::marketable(Side::Buy, 999.0, 6)), 3u);
    const auto trades = sink.of_kind(EventKind::Trade);
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].order_id, 2u);
    EXPECT_EQ(trades[0].quantity, 5);
    EXPECT_EQ(trades[1].order_id, 1u);
    EXPECT_EQ(trades[1].quantity, 1);
    lobtest::expect_healthy(book);
}

LOB_TEST(modify_price_rebooks_order) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 99.0, 5)), 1u);

    lob::ModifyRequest req;
    req.has_new_price = true;
    req.new_price = 98.0;
    EXPECT(book.modify(1u, req));
    EXPECT(book.best_bid().value() == 98.0);
    EXPECT_EQ(book.volume_at(Side::Buy, 99.0), 0);
    EXPECT_EQ(book.volume_at(Side::Buy, 98.0), 5);

    req.new_price = 99.0;
    EXPECT(book.modify(1u, req));
    EXPECT(book.best_bid().value() == 99.0);
    EXPECT_EQ(book.order_count(), 1u);
    lobtest::expect_healthy(book);
}

LOB_TEST(modify_validation_rejects_bad_values) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5, 1)), 1u);

    lob::ModifyRequest bad;
    bad.has_new_quantity = true;
    bad.new_quantity = 0;
    EXPECT(book.modify(1u, bad) == false);
    EXPECT(sink.of_kind(EventKind::Rejected).back().reason != nullptr &&
           std::string(sink.of_kind(EventKind::Rejected).back().reason) ==
               "quantity-must-be-positive");

    bad.has_new_quantity = false;
    bad.has_new_price = true;
    bad.new_price = -1.0;
    EXPECT(book.modify(1u, bad) == false);
    EXPECT(!book.modify(42u, lob::ModifyRequest{}));  // unknown order

    EXPECT_EQ(sink.count(EventKind::Rejected), 3u);
    lobtest::expect_healthy(book);
}

LOB_TEST(submission_validation_rejects) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 0)), 1u);    // qty
    EXPECT_EQ(book.submit(lobtest::limit(Side::Sell, 0.0, 5)), 2u);     // missing limit
    EXPECT_EQ(book.submit(lobtest::stop(Side::Buy, 0.0, 5)), 3u);       // missing trigger
    EXPECT_EQ(book.submit(lobtest::stop_limit(Side::Buy, 100.0, 0.0, 5)), 4u);
    EXPECT_EQ(sink.count(EventKind::Rejected), 4u);
    for (const auto &e : sink.events) {
        EXPECT_EQ(e.kind, EventKind::Rejected);
        EXPECT(e.reason != nullptr);
    }
    EXPECT_EQ(book.order_count(), 0u);
    lobtest::expect_healthy(book);
}

LOB_TEST(duplicate_id_is_rejected) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5, 77)), 77u);
    EXPECT_EQ(book.submit(lobtest::limit(Side::Buy, 100.0, 5, 77)), 77u);
    const auto rejected = sink.of_kind(EventKind::Rejected);
    EXPECT_EQ(rejected.size(), 1u);
    EXPECT(rejected[0].reason != nullptr && std::string(rejected[0].reason) == "duplicate-order-id");
    lobtest::expect_healthy(book);
}

LOB_TEST(engine_ids_never_reuse_provided_ids) {
    lobtest::Recorder sink;
    lob::OrderBook book(&sink);
    EXPECT_EQ(book.submit(lobtest::market(Side::Buy, 1)), 1u);
    EXPECT_EQ(book.submit(lobtest::market(Side::Buy, 1, 5000)), 5000u);
    EXPECT_EQ(book.submit(lobtest::market(Side::Buy, 1)), 5001u);  // skips past 5000 forever
    lobtest::expect_healthy(book);
}
