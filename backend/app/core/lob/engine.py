"""Engine facade: the five public order types driven against the Book."""

from dataclasses import dataclass, replace

from .book import Book
from .contracts import (
    from_ticks,
    OrderError,
    OrderRejectedError,
    OrderStatus,
    OrderType,
    Side,
    to_ticks,
    validate_price,
)
from .events import (
    Accepted,
    Cancelled,
    EngineEvent,
    Modified,
    Rejected,
    StopTriggered,
    Trade,
)
from .nodes import Order  # noqa: F401  (re-exported through package __init__)
from .orders import (
    BaseOrder,
    LimitOrder,
    MarketLimitOrder,
    MarketOrder,
    StopLimitOrder,
    StopOrder,
)


@dataclass(slots=True)
class _Record:
    """Engine-side registration of one accepted order (mutable, internal)."""

    order: BaseOrder
    status: OrderStatus
    sequence: int
    node: Order


class LimitOrderBook:
    """Single-writer engine: submit/modify/cancel/stop updates applied inline.

    No I/O happens in the engine (see the core README storage section).
    """

    def __init__(self) -> None:
        self._book = Book()
        self._records: dict[int, _Record] = {}
        self._next_order_id = 1
        self._next_sequence = 1
        self._last_trade_price: float | None = None

    def _next_event_seq(self) -> int:
        sequence = self._next_sequence
        self._next_sequence += 1
        return sequence

    # -- reads ------------------------------------------------------------

    @property
    def last_trade_price(self) -> float | None:
        return self._last_trade_price

    def get_best_bid_ask(self) -> tuple[float | None, float | None]:
        bid = self._book.get_best_bid()
        ask = self._book.get_best_offer()
        return (
            from_ticks(bid) if bid is not None else None,
            from_ticks(ask) if ask is not None else None,
        )

    def get_best_bid(self) -> float | None:
        bid = self._book.get_best_bid()
        return from_ticks(bid) if bid is not None else None

    def get_best_offer(self) -> float | None:
        ask = self._book.get_best_offer()
        return from_ticks(ask) if ask is not None else None

    def get_volume_at_limit(self, side: Side, price: float) -> int:
        return self._book.get_volume_at_limit(side, to_ticks(price))

    def assert_invariants(self) -> None:
        self._book.assert_invariants()

    # -- submissions ---------------------------------------------------------

    def submit(self, order: BaseOrder) -> list[EngineEvent]:
        """Validate, admit, and execute inline; returns the generated events.

        Stops already past their trigger process immediately; rejected
        limits release their reserved id/sequence entirely.
        """
        if order.order_id is not None:
            return [Rejected(order, "order_id is engine-assigned; submit with order_id=None")]

        order_id = self._next_order_id
        sequence = self._next_sequence
        self._next_order_id += 1
        self._next_sequence += 1
        order._assign_id(order_id)

        if isinstance(order, StopOrder | StopLimitOrder):
            node = _to_node(order, order_id, sequence)
            if (
                self._last_trade_price is not None
                and _stop_fired(order, to_ticks(self._last_trade_price))
            ):
                # Never rests; skip the stop trees and process immediately.
                self._records[order_id] = _Record(
                    order, OrderStatus.TRIGGERED, sequence, node
                )
                converted = _converted_public_order(order)
                events: list[EngineEvent] = [
                    StopTriggered(
                        order_id=order_id, converted=converted, sequence=sequence
                    )
                ]
                self._activate_stop(node)
                events.extend(self._run_node(node, allow_cross=True))
                return events
            self._records[order_id] = _Record(
                order, OrderStatus.PENDING_TRIGGER, sequence, node
            )
            self._book.add_stop(node)
            self._records[order_id].status = OrderStatus.PENDING_TRIGGER
            return [Accepted(order, sequence)]

        try:
            return self._execute_and_rest(order, order_id, sequence)
        except OrderRejectedError as error:
            self._next_order_id -= 1
            self._next_sequence -= 1
            self._records.pop(order_id, None)
            return [Rejected(order, str(error))]

    def _run_node(self, node: Order, *, allow_cross: bool) -> list[EngineEvent]:
        if node.buy_or_sell is Side.BUY:
            if node.order_type is OrderType.MARKET:
                fills = self._book.market_buy(node)
            else:
                fills = self._book.limit_buy(node, allow_cross=allow_cross)[0]
        else:
            if node.order_type is OrderType.MARKET:
                fills = self._book.market_sell(node)
            else:
                fills = self._book.limit_sell(node, allow_cross=allow_cross)[0]

        events = self._emit_trades(fills, node)
        self._finalize_record(node, filled=len(fills) > 0)
        return events

    def _emit_trades(self, fills: list[tuple[Order, int, int]], taker: Order) -> list[EngineEvent]:
        """Trades in firing order + last-traded price; maker records finalized
        so later cancel/modify calls on them are refused correctly."""
        events: list[EngineEvent] = []
        for maker, price_ticks, traded in fills:
            sequence = self._next_sequence
            self._next_sequence += 1
            if taker.buy_or_sell is Side.BUY:
                buy_order, sell_order = taker.id_number, maker.id_number
                buy_agent, sell_agent = taker.agent_id, maker.agent_id
                aggressor = Side.BUY
            else:
                buy_order, sell_order = maker.id_number, taker.id_number
                buy_agent, sell_agent = maker.agent_id, taker.agent_id
                aggressor = Side.SELL
            events.append(
                Trade(
                    sequence=sequence,
                    buy_order_id=buy_order,
                    sell_order_id=sell_order,
                    buy_agent=buy_agent,
                    sell_agent=sell_agent,
                    price=from_ticks(price_ticks),
                    quantity=traded,
                    aggressor_side=aggressor,
                )
            )
            self._last_trade_price = from_ticks(price_ticks)
            record = self._records[maker.id_number]
            if maker.shares == 0 and record.status is OrderStatus.ACTIVE:
                record.status = OrderStatus.FILLED
        return events

    def _execute_and_rest(
        self, order: BaseOrder, order_id: int, sequence: int
    ) -> list[EngineEvent]:
        node = _to_node(order, order_id, sequence)
        self._records[order_id] = _Record(order, OrderStatus.ACTIVE, sequence, node)
        events: list[EngineEvent] = [Accepted(order, sequence)]
        events.extend(self._run_node(node, allow_cross=not isinstance(order, LimitOrder)))
        return events

    def _activate_stop(self, node: Order) -> None:
        """Rekey a triggered stop into working semantics (may cross on entry)."""
        if node.order_type is OrderType.STOP:
            node.order_type = OrderType.MARKET
            node.limit = 0
        else:
            assert node.order_type is OrderType.STOP_LIMIT
            assert node.conversion_limit is not None, "stop-limit without conversion limit"
            node.order_type = OrderType.LIMIT
            node.limit = node.conversion_limit
            node.conversion_limit = None

    def _finalize_record(self, node: Order, *, filled: bool) -> None:
        record = self._records[node.id_number]
        if node.order_type is OrderType.MARKET:
            record.status = OrderStatus.FILLED if node.shares == 0 else OrderStatus.CANCELLED
        elif node.shares > 0:
            record.status = OrderStatus.ACTIVE
        else:
            record.status = OrderStatus.FILLED

    # -- stop trigger evaluation ----------------------------------------------

    def update_market_price(self, price: float) -> list[EngineEvent]:
        """Feed the latest trade price; stops evaluate in firing order."""
        if price <= 0:
            raise OrderError(f"market price must be positive, got {price}")

        events: list[EngineEvent] = []
        for node in self._book.triggered_stops(to_ticks(price)):
            record = self._records[node.id_number]
            assert (
                isinstance(record.order, StopOrder | StopLimitOrder)
                and record.status is OrderStatus.PENDING_TRIGGER
            ), "triggered node is not a pending stop"
            record.status = OrderStatus.TRIGGERED
            events.append(
                StopTriggered(
                    order_id=node.id_number,
                    converted=_converted_public_order(record.order),
                    sequence=self._next_event_seq(),
                )
            )
            self._activate_stop(node)
            events.extend(self._run_node(node, allow_cross=True))
        if not any(isinstance(event, Trade) for event in events):
            self._last_trade_price = price
        return events

    # -- modify / cancel --------------------------------------------------------

    def cancel(self, *, order_id: int, agent_id: str) -> Cancelled | Rejected:
        record = self._records.get(order_id)
        if record is None:
            return Rejected(None, f"unknown order_id {order_id}")
        if record.order.agent_id != agent_id:
            return Rejected(None, f"order {order_id} does not belong to agent {agent_id}")
        working_type = record.node.order_type
        if record.status is OrderStatus.PENDING_TRIGGER:
            self._book.cancel(record.node)
            record.status = OrderStatus.CANCELLED
            return Cancelled(order_id, sequence=self._next_event_seq())
        if record.status is OrderStatus.ACTIVE and working_type in (
            OrderType.LIMIT,
            OrderType.MARKET_LIMIT,
        ):
            self._book.cancel(record.node)
            record.status = OrderStatus.CANCELLED
            return Cancelled(order_id, sequence=self._next_event_seq())
        return Rejected(
            record.order, f"{working_type} in status {record.status} is not cancellable"
        )

    def modify(
        self,
        *,
        order_id: int,
        agent_id: str,
        quantity: int | None = None,
        limit_price: float | None = None,
        stop_price: float | None = None,
    ) -> Modified | Rejected:
        """Patch quantity/limit/stop on modifiable orders.

        ``quantity`` sets the working (remaining) size. Price changes rekey
        (cancel + reinsert, same id, fresh sequence); crossing rekeys and
        fire-on-modify stop prices are refused.
        """
        record = self._records.get(order_id)
        if record is None:
            return Rejected(None, f"unknown order_id {order_id}")
        if record.order.agent_id != agent_id:
            return Rejected(None, f"order {order_id} does not belong to agent {agent_id}")
        if record.status not in (OrderStatus.ACTIVE, OrderStatus.PENDING_TRIGGER):
            return Rejected(
                record.order, f"order {order_id} is {record.status}; not modifiable"
            )

        working_type = record.node.order_type
        allowed = _modifiable_fields(working_type)
        requested: dict[str, float | int] = {}
        if quantity is not None:
            if not isinstance(quantity, int) or quantity <= 0:
                return Rejected(record.order, "quantity must be a positive int")
            requested["quantity"] = quantity
        if limit_price is not None:
            try:
                validate_price("limit_price", limit_price)
            except OrderError as error:
                return Rejected(record.order, str(error))
            requested["limit_price"] = limit_price
        if stop_price is not None:
            try:
                validate_price("stop_price", stop_price)
            except OrderError as error:
                return Rejected(record.order, str(error))
            requested["stop_price"] = stop_price
        if not requested:
            return Rejected(record.order, "no fields passed to modify")
        for name in requested:
            if name not in allowed:
                return Rejected(
                    record.order,
                    f"{record.order.order_type} does not support modifying '{name}'",
                )

        # A modified stop price that would instantly trigger is refused.
        if (
            record.status is OrderStatus.PENDING_TRIGGER
            and stop_price is not None
            and self._last_trade_price is not None
        ):
            market_ticks = to_ticks(self._last_trade_price)
            proposed_ticks = to_ticks(stop_price)
            fired_now = (
                record.order.side is Side.BUY and market_ticks >= proposed_ticks
            ) or (record.order.side is Side.SELL and market_ticks <= proposed_ticks)
            if fired_now:
                return Rejected(
                    record.order,
                    "new stop price would trigger immediately; cancel and resubmit",
                )

        limit_price_changed = (
            limit_price is not None
            and record.status is OrderStatus.ACTIVE
            and working_type is OrderType.LIMIT
        )
        stop_price_changed = (
            stop_price is not None and record.status is OrderStatus.PENDING_TRIGGER
        )

        if limit_price_changed or stop_price_changed:
            # Rekey: cross-check BEFORE any mutation, so a refusal leaves the
            # engine untouched; time priority is lost (fresh sequence).
            if limit_price_changed:
                try:
                    self._assert_rekey_does_not_cross(updated := replace(record.order, **requested))
                except OrderRejectedError as error:
                    return Rejected(record.order, str(error))
            else:
                updated = replace(record.order, **requested)
            sequence = self._next_sequence
            self._next_sequence += 1
            self._book.cancel(record.node)
            if record.status is OrderStatus.PENDING_TRIGGER:
                new_node = _to_node(updated, order_id, sequence)
                self._book.add_stop(new_node)
                record.order = updated
                record.sequence = sequence
                record.node = new_node
            else:
                working_shares = requested.get("quantity", record.node.shares)
                new_node = _limit_node_for_rekey(updated, order_id, sequence, working_shares)
                self._book._rest(new_node, new_node.buy_or_sell)
                record.order = updated
                record.sequence = sequence
                record.node = new_node
            return Modified(updated, sequence=sequence)

        # In-place pure quantity update.
        node = record.node
        if quantity is not None:
            level = node.parent_limit
            assert level is not None, "modifiable order must be on a level"
            level.total_volume += quantity - node.shares
            node.shares = quantity
        record.order = replace(record.order, **requested)
        return Modified(record.order, sequence=self._next_event_seq())

    def _assert_rekey_does_not_cross(self, updated: BaseOrder) -> None:
        """A rekeyed resting limit must not cross (matches Add policy)."""
        if not isinstance(updated, LimitOrder):
            return
        if updated.side is Side.BUY:
            ask = self._book.lowest_sell
            if ask is not None and updated.limit_ticks >= ask.limit_price:
                raise OrderRejectedError("modified price would cross the book")
        else:
            bid = self._book.highest_buy
            if bid is not None and updated.limit_ticks <= bid.limit_price:
                raise OrderRejectedError("modified price would cross the book")


def _to_node(order: BaseOrder, order_id: int, sequence: int) -> Order:
    """Convert a public order type into a working node for the trees."""
    if isinstance(order, MarketOrder):
        return Order(
            id_number=order_id,
            buy_or_sell=order.side,
            shares=order.quantity,
            limit=0,
            order_type=OrderType.MARKET,
            agent_id=order.agent_id,
            sequence=sequence,
        )
    if isinstance(order, LimitOrder):
        return Order(
            id_number=order_id,
            buy_or_sell=order.side,
            shares=order.quantity,
            limit=order.limit_ticks,
            order_type=OrderType.LIMIT,
            agent_id=order.agent_id,
            sequence=sequence,
        )
    if isinstance(order, MarketLimitOrder):
        return Order(
            id_number=order_id,
            buy_or_sell=order.side,
            shares=order.quantity,
            limit=order.limit_ticks,
            order_type=OrderType.MARKET_LIMIT,
            agent_id=order.agent_id,
            sequence=sequence,
        )
    if isinstance(order, StopOrder):
        return Order(
            id_number=order_id,
            buy_or_sell=order.side,
            shares=order.quantity,
            limit=order.stop_ticks,
            order_type=OrderType.STOP,
            agent_id=order.agent_id,
            sequence=sequence,
        )
    assert isinstance(order, StopLimitOrder)
    return Order(
        id_number=order_id,
        buy_or_sell=order.side,
        shares=order.quantity,
        limit=order.stop_ticks,
        order_type=OrderType.STOP_LIMIT,
        agent_id=order.agent_id,
        sequence=sequence,
        conversion_limit=order.limit_ticks,
    )


def _stop_fired(order: StopOrder | StopLimitOrder, market_price: int) -> bool:
    """BUY fires when price >= stop; SELL when price <= stop."""
    if order.side is Side.BUY:
        return market_price >= order.stop_ticks
    return market_price <= order.stop_ticks


def _converted_public_order(order: StopOrder | StopLimitOrder) -> BaseOrder:
    """The public representation of a converted stop order."""
    if isinstance(order, StopOrder):
        return MarketOrder(
            agent_id=order.agent_id,
            side=order.side,
            quantity=order.quantity,
            order_id=order.order_id,
        )
    return MarketLimitOrder(
        agent_id=order.agent_id,
        side=order.side,
        quantity=order.quantity,
        limit_price=order.limit_price,
        order_id=order.order_id,
    )


def _modifiable_fields(working_type: OrderType) -> set[str]:
    """Field names an in-flight order accepts into modify()."""
    fields = {"quantity"}
    if working_type is OrderType.LIMIT:
        fields.add("limit_price")
    elif working_type is OrderType.STOP:
        fields.add("stop_price")
    elif working_type is OrderType.STOP_LIMIT:
        fields |= {"limit_price", "stop_price"}
    return fields


def _limit_node_for_rekey(
    updated: LimitOrder, order_id: int, sequence: int, working_shares: int
) -> Order:
    return Order(
        id_number=order_id,
        buy_or_sell=updated.side,
        shares=working_shares,
        limit=updated.limit_ticks,
        order_type=OrderType.LIMIT,
        agent_id=updated.agent_id,
        sequence=sequence,
    )
