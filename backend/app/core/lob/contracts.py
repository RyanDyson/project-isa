"""Shared enums, errors, and tick math for the LOB engine."""

from enum import StrEnum

class Side(StrEnum):
    BUY = "buy"
    SELL = "sell"

class OrderType(StrEnum):
    MARKET = "market"
    LIMIT = "limit"
    MARKET_LIMIT = "market-limit"
    STOP = "stop"
    STOP_LIMIT = "stop-limit"

class OrderStatus(StrEnum):
    PENDING_TRIGGER = "pending-trigger"
    ACTIVE = "active"
    TRIGGERED = "triggered"
    CANCELLED = "cancelled"
    FILLED = "filled"

class OrderError(ValueError):
    """Raised when an order's fields are invalid at construction time."""

class OrderRejectedError(OrderError):
    """Policy refusal from book entry points; converted to Rejected."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason

PRICE_TICK = 0.01
_PRICE_EPSILON = 1e-9

def to_ticks(price: float) -> int:
    """Convert a public float price to engine-internal integer ticks."""
    ticks = round(price / PRICE_TICK)
    if abs(ticks * PRICE_TICK - price) > _PRICE_EPSILON:
        raise OrderError(f"price {price} must be a multiple of the tick size {PRICE_TICK}")
    return ticks

def from_ticks(ticks: int) -> float:
    """Convert integer ticks back to the public float representation."""
    return ticks * PRICE_TICK


def validate_price(name: str, value: float) -> None:
    if value <= 0:
        raise OrderError(f"{name} must be positive, got {value}")
    try:
        to_ticks(value)
    except OrderError as error:
        raise OrderError(f"{name}: {error}") from None
