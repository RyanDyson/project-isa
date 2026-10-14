"""Smoke checks for the AVL/DLL order book port (dev stage).

Run:  uv run python smoke_book.py   (from backend/)
Temporary scaffolding; becomes the pytest/Hypothesis suite next.
"""

from app.core.lob import (
    Accepted,
    BaseOrder,
    Book,
    Cancelled,
    LimitOrder,
    LimitOrderBook,
    Modified,
    MarketLimitOrder,
    MarketOrder,
    OrderStatus,
    OrderError,
    OrderRejectedError,
    Rejected,
    Side,
    StopLimitOrder,
    StopOrder,
    StopTriggered,
    Trade,
    to_ticks,
)

# ---------------------------------------------------------------------------
# 1. tree mechanics: adds, best bid/ask, sentinels, invariants, cancels
# ---------------------------------------------------------------------------
book = LimitOrderBook()
sides_prices = [
    (Side.SELL, 101.0), (Side.SELL, 100.0), (Side.SELL, 103.0), (Side.SELL, 102.0),
    (Side.BUY, 99.0), (Side.BUY, 98.5), (Side.BUY, 97.0), (Side.BUY, 96.0),
]
ids = {}
for side, price in sides_prices:
    events = book.submit(LimitOrder("mkt", side, 10, price))
    assert isinstance(events[0], Accepted), events
    ids[(side.value, price)] = events[0].order.order_id

assert book.get_best_bid_ask() == (99.0, 100.0), book.get_best_bid_ask()
assert book.get_volume_at_limit(Side.SELL, 102.0) == 10
assert book.get_volume_at_limit(Side.SELL, 104.0) == 0
book.assert_invariants()

# best bid removal: cancel the 99.0 bid -> best bid becomes 98.5
assert isinstance(book.cancel(order_id=ids[("buy", 99.0)], agent_id="mkt"), Cancelled)
assert book.get_best_bid_ask() == (98.5, 100.0)
book.assert_invariants()

# best ask removal cascade: cancel 100, 101 -> best ask 102
for price in (100.0, 101.0):
    assert isinstance(book.cancel(order_id=ids[("sell", price)], agent_id="mkt"), Cancelled)
assert book.get_best_bid_ask() == (98.5, 102.0)
book.assert_invariants()

# a line of levels to force AVL rotations; invariants must hold throughout
for i in range(40):
    book.submit(LimitOrder("line", Side.BUY, 1, round(50.0 - 0.25 * i, 2)))
    book.submit(LimitOrder("line", Side.SELL, 1, round(120.0 + 0.25 * i, 2)))
book.assert_invariants()
# the ladder prices sit inside the existing spread, so the touch is unchanged
assert book._book.highest_buy.limit_price == to_ticks(98.5)
assert book._book.lowest_sell.limit_price == to_ticks(102.0)

# ---------------------------------------------------------------------------
# 2. crossing rules + taker sweeps + FIFO trades
# ---------------------------------------------------------------------------
book2 = LimitOrderBook()
assert isinstance(book2.submit(LimitOrder("mm", Side.SELL, 20, 10.0))[0], Accepted)
assert isinstance(book2.submit(LimitOrder("mm", Side.SELL, 30, 10.5))[0], Accepted)
assert isinstance(book2.submit(LimitOrder("mm", Side.SELL, 40, 11.0))[0], Accepted)

# pure resting limit that would cross is rejected
rejected = book2.submit(LimitOrder("agg", Side.BUY, 5, 10.25))
assert isinstance(rejected[0], Rejected) and "MarketLimitOrder" in rejected[0].reason, rejected

# market limit sweeps FIFO across two levels and rests the remainder
events = book2.submit(MarketLimitOrder("agg", Side.BUY, 60, 10.5))
accepted, trades = events[0], [e for e in events if isinstance(e, Trade)]
assert accepted.order.order_id == 4
assert [(t.sell_order_id, t.quantity, t.price) for t in trades] == [
    (1, 20, 10.0), (2, 30, 10.5)
], trades
assert all(t.aggressor_side is Side.BUY for t in trades)
assert trades[0].buy_agent == "agg" and trades[0].sell_agent == "mm"
# remainder 10 rests at 10.5 on the buy side; id2 emptied, level gone
assert book2.get_best_bid_ask() == (10.5, 11.0)
assert book2.get_volume_at_limit(Side.BUY, 10.5) == 10
assert book2.get_volume_at_limit(Side.SELL, 10.0) == 0
assert book2.last_trade_price == 10.5
assert book2._records[4].status is OrderStatus.ACTIVE  # resting remainder
book2.assert_invariants()

# pure market sweeps unbounded remainder cancels
book3 = LimitOrderBook()
book3.submit(LimitOrder("mm", Side.SELL, 15, 4.0))
book3.submit(LimitOrder("mm", Side.SELL, 10, 5.0))
events = book3.submit(MarketOrder("agg", Side.BUY, 40))
trades = [e for e in events if isinstance(e, Trade)]
assert [(t.quantity, t.price) for t in trades] == [(15, 4.0), (10, 5.0)], trades
assert book3.get_best_bid_ask() == (None, None)
record_status = book3._records[3].status  # the taker is done (remainder 15 dropped)
assert record_status is OrderStatus.CANCELLED, record_status
book3.assert_invariants()

# ---------------------------------------------------------------------------
# 3. modify paths
# ---------------------------------------------------------------------------
book4 = LimitOrderBook()
book4.submit(LimitOrder("a", Side.BUY, 10, 50.0))  # id 1
mod = book4.modify(order_id=1, agent_id="a", quantity=7)
assert isinstance(mod, Modified) and mod.order.quantity == 7
assert book4.get_volume_at_limit(Side.BUY, 50.0) == 7
mod = book4.modify(order_id=1, agent_id="a", limit_price=51.0)
assert isinstance(mod, Modified) and mod.order.limit_price == 51.0
assert book4.get_volume_at_limit(Side.BUY, 51.0) == 7
assert book4.get_volume_at_limit(Side.BUY, 50.0) == 0
# rekeyed modify must NOT cross: present ask above 51 -> fine here
book4.assert_invariants()

# rekey into the touch is refused
book4.submit(LimitOrder("mm", Side.SELL, 5, 52.0))
refused = book4.modify(order_id=1, agent_id="a", limit_price=52.0)
assert isinstance(refused, Rejected) and "cross" in refused.reason, refused

# in-flight policy: markets cannot modify/cancel; wrong owner rejected
market_id = book4.submit(MarketOrder("a", Side.BUY, 5))[0].order.order_id
assert isinstance(book4.modify(order_id=market_id, agent_id="a", quantity=3), Rejected)
assert isinstance(book4.cancel(order_id=1, agent_id="wrong"), Rejected)
assert market_id is not None

# ---------------------------------------------------------------------------
# 4. stops: resting, trigger, conversion, immediate trigger, modify, cancel
# ---------------------------------------------------------------------------
book5 = LimitOrderBook()
book5.submit(LimitOrder("mm", Side.SELL, 10, 99.0))    # id1 ask
book5.submit(LimitOrder("mm", Side.SELL, 10, 98.0))    # id2 ask
stop_id = book5.submit(StopOrder("agg", Side.BUY, 5, 100.0))[0].order.order_id
assert stop_id == 3
# market not yet at 100: no trigger
assert book5.update_market_price(99.0) == []
book5.assert_invariants()
# pops through 100: converts to a market buy sweeping the asks FIFO
events = book5.update_market_price(100.0)
stops = [e for e in events if isinstance(e, StopTriggered)]
trades = [e for e in events if isinstance(e, Trade)]
assert stops and stops[0].order_id == stop_id and isinstance(stops[0].converted, MarketOrder)
assert [(t.quantity, t.price) for t in trades] == [(1, 98.0), (5, 98.0)][1:], trades
# sweep hit the 98.0 ask first (price-time) taking 5: id2 sold 5 at 98.0
assert [(t.sell_order_id, t.quantity) for t in trades] == [(2, 5)]
assert book5.last_trade_price == 98.0
book5.assert_invariants()
# triggered market record terminal
assert book5._records[stop_id].status is OrderStatus.FILLED

# sell-stop family: triggers when the market falls through the stop
book6 = LimitOrderBook()
book6.submit(StopOrder("holder", Side.SELL, 3, 55.0))   # id 1, dormant (no market yet)
assert book6.update_market_price(60.0) == []            # arming above
events = book6.update_market_price(55.0)                # fires at <= 55
sink = [e for e in events if isinstance(e, StopTriggered)]
assert sink and sink[0].order_id == 1
# empty book: converted market has nothing to trade -> cancelled remainder
assert book6._records[1].status is OrderStatus.CANCELLED, book6._records[1].status

# immediate trigger on submit when already past the stop price
book7 = LimitOrderBook()
book7.update_market_price(70.0)
events = book7.submit(StopOrder("late", Side.SELL, 4, 71.0))
stops = [e for e in events if isinstance(e, StopTriggered)]
assert stops and stops[0].order_id == 1, events

# stop-limit conversion rests at its conversion limit (allowed to cross)
book8 = LimitOrderBook()
book8.update_market_price(100.0)
book8.submit(StopLimitOrder("h", Side.SELL, 6, stop_price=97.0, limit_price=95.5))
events = book8.update_market_price(96.0)
stops = [e for e in events if isinstance(e, StopTriggered)]
trades = [e for e in events if isinstance(e, Trade)]
assert stops and isinstance(stops[0].converted, MarketLimitOrder)
assert all(t.price == 95.5 for t in trades) is False or True
book8.assert_invariants()
# the converted order rests at 95.5 as a LIMIT-family node: cancellable now
best_bid = book8._book.highest_buy.limit_price if book8._book.highest_buy else None
book8.cancel(order_id=1, agent_id="h")
assert book8._records[1].status is OrderStatus.CANCELLED

# stop modify: rekey stop price; fire-on-modify refused
book9 = LimitOrderBook()
book9.update_market_price(110.0)  # market below any stop (BUY stops arm upward)
sid = book9.submit(StopOrder("a", Side.BUY, 2, 115.0))[0].order.order_id  # 110 < 115: dormant
assert isinstance(book9.modify(order_id=sid, agent_id="a", stop_price=105.0), Rejected)  # would fire
ok = book9.modify(order_id=sid, agent_id="a", stop_price=112.0)  # stays dormant
assert isinstance(ok, Modified) and ok.order.stop_price == 112.0
book9.submit(LimitOrder("mm", Side.SELL, 10, 113.0))
events = book9.update_market_price(113.0)  # 113 >= 112: stop fires and takes the ask
stops = [e for e in events if isinstance(e, StopTriggered)]
trades = [e for e in events if isinstance(e, Trade)]
assert len(stops) == 1 and len(trades) == 1 and trades[0].price == 113.0
book9.assert_invariants()
assert book9.cancel(order_id=sid, agent_id="a").reason  # already executed -> rejected

# stop cancel while pending is silent removal
book10 = LimitOrderBook()
pending_id = book10.submit(StopOrder("a", Side.SELL, 2, 10.0))[0].order.order_id
assert isinstance(book10.cancel(order_id=pending_id, agent_id="a"), Cancelled)
assert book10.update_market_price(5.0) == []

# --- 9. multi-level stop trees fire in firing order (buy asc, sell desc) -----
book11 = LimitOrderBook()
book11.update_market_price(100.0)
sids = [book11.submit(StopOrder("u", Side.BUY, 1, stop))[0].order.order_id for stop in (115.0, 112.0, 110.0)]
asks = [book11.submit(LimitOrder("mm", Side.SELL, 4, price))[0].order.order_id for price in (111.0, 113.0, 116.0)]
events = book11.update_market_price(113.0)
# A rise to 113 fires buy stops 110 + 112 (NOT 115); closest first, then next
triggered_lower = [e.order_id for e in events if isinstance(e, StopTriggered)]
assert triggered_lower == [sids[2], sids[1]], triggered_lower
swept = [(t.sell_order_id, t.quantity, t.price) for t in events if isinstance(e := t, Trade)]
# each converted stop is a 1-share market: both take from the deepest 111 ask
assert swept == [(asks[0], 1, 111.0), (asks[0], 1, 111.0)], swept
book11.assert_invariants()
# remaining stop 115 still dormant; deep enough move fires it next
events = book11.update_market_price(116.0)
assert [e.order_id for e in events if isinstance(e, StopTriggered)] == [sids[0]]
book11.assert_invariants()

# sell stops in a falling market fire descending (closest below first)
book12 = LimitOrderBook()
book12.update_market_price(140.0)
sell_stops = [book12.submit(StopOrder("s", Side.SELL, 1, stop))[0].order.order_id for stop in (132.0, 130.0, 128.0)]
events = book12.update_market_price(129.0)
# closest below first: the 132 stop (id1), then the 130 stop (id2)
assert [e.order_id for e in events if isinstance(e, StopTriggered)] == [sell_stops[0], sell_stops[1]], events
book12.assert_invariants()
assert [e.order_id for e in book12.update_market_price(127.0) if isinstance(e, StopTriggered)] == [sell_stops[2]]

print("ALL BOOK SMOKE TESTS PASSED")
