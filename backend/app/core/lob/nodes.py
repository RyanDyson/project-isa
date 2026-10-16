"""Internal book structures: the ported C++ Order and Limit structs."""

from __future__ import annotations

from typing import Iterator

from .contracts import OrderType, Side


class Order:
    """One working order node inside the book.

    Doubly linked within its price-level FIFO. ``limit`` is the node key in
    ticks: limit price for resting orders, stop price for pending stop nodes
    (a stop-limit's working limit price lives in ``conversion_limit`` until
    conversion).
    """

    __slots__ = (
        "id_number",
        "buy_or_sell",
        "shares",
        "limit",
        "next_order",
        "prev_order",
        "parent_limit",
        "order_type",
        "agent_id",
        "sequence",
        "conversion_limit",
    )

    def __init__(
        self,
        id_number: int,
        buy_or_sell: Side,
        shares: int,
        limit: int,
        order_type: OrderType,
        agent_id: str,
        sequence: int,
        conversion_limit: int | None = None,
    ) -> None:
        self.id_number = id_number
        self.buy_or_sell = buy_or_sell
        self.shares = shares
        self.limit = limit
        self.next_order: Order | None = None
        self.prev_order: Order | None = None
        self.parent_limit: Limit | None = None
        self.order_type = order_type
        self.agent_id = agent_id
        self.sequence = sequence
        self.conversion_limit = conversion_limit

    def __repr__(self) -> str:  # pragma: no cover
        return (
            f"Order(id={self.id_number}, type={self.order_type.value}, "
            f"side={self.buy_or_sell.value}, shares={self.shares}, "
            f"limit={self.limit})"
        )


class Limit:
    """One price level owning a FIFO order list; aggregates read O(1)."""

    __slots__ = ("limit_price", "size", "total_volume", "head_order", "tail_order")

    def __init__(self, limit_price: int) -> None:
        self.limit_price = limit_price
        self.size = 0
        self.total_volume = 0
        self.head_order: Order | None = None
        self.tail_order: Order | None = None

    def append(self, order: Order) -> None:
        order.parent_limit = self
        order.prev_order = self.tail_order
        order.next_order = None
        if self.tail_order is None:
            self.head_order = order
        else:
            self.tail_order.next_order = order
        self.tail_order = order
        self.size += 1
        self.total_volume += order.shares

    def unlink(self, order: Order, *, shares_left: bool) -> None:
        """Relink out of the FIFO; ``shares_left`` moves resting shares out
        of the aggregate (full fills already decremented)."""
        if order.prev_order is not None:
            order.prev_order.next_order = order.next_order
        else:
            self.head_order = order.next_order
        if order.next_order is not None:
            order.next_order.prev_order = order.prev_order
        else:
            self.tail_order = order.prev_order
        if shares_left:
            self.total_volume -= order.shares
        order.next_order = None
        order.prev_order = None
        order.parent_limit = None
        self.size -= 1


def iter_fifo(level: Limit) -> Iterator[Order]:
    node = level.head_order
    while node is not None:
        yield node
        node = node.next_order
