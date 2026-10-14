"""Engine-facing value objects: the events the engine emits per action."""

from dataclasses import dataclass

from .contracts import Side
from .orders import BaseOrder


@dataclass(frozen=True, slots=True)
class Accepted:
    order: BaseOrder
    sequence: int


@dataclass(frozen=True, slots=True)
class Rejected:
    order: BaseOrder | None
    reason: str


@dataclass(frozen=True, slots=True)
class Modified:
    order: BaseOrder


@dataclass(frozen=True, slots=True)
class Cancelled:
    order_id: int


@dataclass(frozen=True, slots=True)
class StopTriggered:
    order_id: int
    converted: BaseOrder


@dataclass(frozen=True, slots=True)
class Trade:
    """One fill; both sides recorded for portfolio attribution."""

    sequence: int
    buy_order_id: int
    sell_order_id: int
    buy_agent: str
    sell_agent: str
    price: float
    quantity: int
    aggressor_side: Side


EngineEvent = Accepted | Rejected | Modified | Cancelled | StopTriggered | Trade
