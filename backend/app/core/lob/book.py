"""Book: four SortedDict price ladders plus market-adjacent sentinels.

`bids`/`asks` hold resting limits, `stop_buys`/`stop_sells` pending stops;
keys are price ticks and values are owning Limit levels. SortedDict keeps
levels ordered (min/max peek O(1)-ish, insert/delete O(log n)), replacing
the hand-rolled AVL (see pyproject: sortedcontainers).
"""

from sortedcontainers import SortedDict

from .contracts import OrderRejectedError, OrderType, Side
from .nodes import Limit, Order, iter_fifo


class Book:
    def __init__(self) -> None:
        self.bids: SortedDict[int, Limit] = SortedDict()
        self.asks: SortedDict[int, Limit] = SortedDict()
        self.stop_buys: SortedDict[int, Limit] = SortedDict()
        self.stop_sells: SortedDict[int, Limit] = SortedDict()

        self.highest_buy: Limit | None = None
        self.lowest_sell: Limit | None = None
        self.lowest_stop_buy: Limit | None = None
        self.highest_stop_sell: Limit | None = None

    def _resting(self, side: Side) -> SortedDict[int, Limit]:
        return self.bids if side is Side.BUY else self.asks

    def _stops(self, side: Side) -> SortedDict[int, Limit]:
        return self.stop_buys if side is Side.BUY else self.stop_sells

    def _refresh_sentinels(self) -> None:
        self.highest_buy = self.bids.peekitem(-1)[1] if self.bids else None
        self.lowest_sell = self.asks.peekitem(0)[1] if self.asks else None
        self.lowest_stop_buy = self.stop_buys.peekitem(0)[1] if self.stop_buys else None
        self.highest_stop_sell = self.stop_sells.peekitem(-1)[1] if self.stop_sells else None

    def limit_buy(
        self, order: Order, *, allow_cross: bool
    ) -> tuple[list[tuple[Order, int, int]], Order | None]:
        """Returns ``(fills, resting_node)``; fills are
        ``(maker_order, price_ticks, traded_qty)``."""
        if not allow_cross:
            ask = self.lowest_sell
            if ask is not None and order.limit >= ask.limit_price:
                raise OrderRejectedError("buy limit would cross the book; use MarketLimitOrder")
        fills, remainder = self._take(order)
        if remainder > 0:
            self._rest(order, Side.BUY)
            return fills, order
        return fills, None

    def limit_sell(
        self, order: Order, *, allow_cross: bool
    ) -> tuple[list[tuple[Order, int, int]], Order | None]:
        if not allow_cross:
            bid = self.highest_buy
            if bid is not None and order.limit <= bid.limit_price:
                raise OrderRejectedError("sell limit would cross the book; use MarketLimitOrder")
        fills, remainder = self._take(order)
        if remainder > 0:
            self._rest(order, Side.SELL)
            return fills, order
        return fills, None

    def market_buy(self, order: Order) -> list[tuple[Order, int, int]]:
        fills, _ = self._take(order)
        return fills

    def market_sell(self, order: Order) -> list[tuple[Order, int, int]]:
        fills, _ = self._take(order)
        return fills

    def _take(self, incoming: Order) -> tuple[list[tuple[Order, int, int]], int]:
        """Taker sweep against best opposite-side levels, FIFO within level.

        Sweep is price-bounded except for market orders; the remainder stays
        on ``incoming.shares`` for the caller to rest or drop.
        """
        buy_side = incoming.buy_or_sell is Side.BUY
        opposite_side = Side.SELL if buy_side else Side.BUY
        bound = incoming.limit if incoming.order_type is not OrderType.MARKET else None
        ladder = self.asks if buy_side else self.bids
        fills: list[tuple[Order, int, int]] = []
        remaining = incoming.shares

        while remaining > 0:
            if not ladder:
                break
            price_ticks, best = ladder.peekitem(0 if buy_side else -1)
            if bound is not None and (
                (buy_side and price_ticks > bound) or (not buy_side and price_ticks < bound)
            ):
                break

            head = best.head_order
            assert head is not None
            while head is not None and remaining > 0:
                traded = min(head.shares, remaining)
                fills.append((head, price_ticks, traded))
                remaining -= traded
                head.shares -= traded
                best.total_volume -= traded
                if head.shares == 0:
                    best.unlink(head, shares_left=False)
                    if best.size == 0:
                        del ladder[price_ticks]
                        self._refresh_sentinels()
                head = best.head_order

        incoming.shares = remaining
        return fills, remaining

    def _rest(self, order: Order, side: Side) -> None:
        ladder = self._resting(side)
        level = ladder.get(order.limit)
        if level is None:
            level = Limit(order.limit)
            ladder[order.limit] = level
            self._refresh_sentinels()
        level.append(order)

    def cancel(self, order: Order) -> None:
        level = order.parent_limit
        if level is None:
            raise AssertionError(f"order {order.id_number} is not on any level")
        stop_tree = order.order_type in (OrderType.STOP, OrderType.STOP_LIMIT)
        ladder = self._stops(order.buy_or_sell) if stop_tree else self._resting(order.buy_or_sell)
        level.unlink(order, shares_left=True)
        if level.size == 0:
            del ladder[level.limit_price]
            self._refresh_sentinels()

    def add_stop(self, order: Order) -> None:
        ladder = self._stops(order.buy_or_sell)
        level = ladder.get(order.limit)
        if level is None:
            level = Limit(order.limit)
            ladder[order.limit] = level
            self._refresh_sentinels()
        level.append(order)

    def triggered_stops(self, market_price: int) -> list[Order]:
        """Buy stops fire ascending (market rising), sell stops descending."""
        triggered: list[Order] = []
        while self.stop_buys:
            price_ticks, level = self.stop_buys.peekitem(0)
            if market_price < price_ticks:
                break
            del self.stop_buys[price_ticks]
            self._refresh_sentinels()
            triggered.extend(iter_fifo(level))
            level.head_order = level.tail_order = None
        while self.stop_sells:
            price_ticks, level = self.stop_sells.peekitem(-1)
            if market_price > price_ticks:
                break
            del self.stop_sells[price_ticks]
            self._refresh_sentinels()
            triggered.extend(iter_fifo(level))
            level.head_order = level.tail_order = None
        return triggered

    def get_best_bid(self) -> int | None:
        return self.highest_buy.limit_price if self.highest_buy is not None else None

    def get_best_offer(self) -> int | None:
        return self.lowest_sell.limit_price if self.lowest_sell is not None else None

    def get_volume_at_limit(self, side: Side, price: int) -> int:
        level = self._resting(side).get(price)
        return level.total_volume if level is not None else 0

    def assert_invariants(self) -> None:
        """Validate FIFO consistency, aggregates, and sentinels per ladder."""
        ladders: list[tuple[str, SortedDict[int, Limit]]] = [
            ("bids", self.bids),
            ("asks", self.asks),
            ("stop_buys", self.stop_buys),
            ("stop_sells", self.stop_sells),
        ]
        for name, ladder in ladders:
            for price_ticks, level in ladder.items():
                assert level.limit_price == price_ticks, (
                    f"{name}: level key mismatch at {price_ticks}"
                )
                counted = 0
                volume = 0
                prev: Order | None = None
                cursor = level.head_order
                while cursor is not None:
                    assert cursor.prev_order is prev
                    assert cursor.parent_limit is level
                    counted += 1
                    volume += cursor.shares
                    prev, cursor = cursor, cursor.next_order
                assert level.tail_order is prev, f"{name}: tail pointer wrong at {price_ticks}"
                assert level.size == counted and level.total_volume == volume, (
                    f"{name}: aggregates off at {price_ticks}"
                )

        self._check_sentinel(self.bids, self.highest_buy, -1)
        self._check_sentinel(self.asks, self.lowest_sell, 0)
        self._check_sentinel(self.stop_buys, self.lowest_stop_buy, 0)
        self._check_sentinel(self.stop_sells, self.highest_stop_sell, -1)

    def _check_sentinel(self, ladder: SortedDict[int, Limit], sentinel: Limit | None, position: int) -> None:
        extreme = ladder.peekitem(position)[1] if ladder else None
        assert extreme is sentinel, "sentinel pointer drifted"
