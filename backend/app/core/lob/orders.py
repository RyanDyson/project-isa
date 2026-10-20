"""The submission model: five public order types agents may send."""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field

from .contracts import OrderError, OrderType, Side, to_ticks, validate_price


@dataclass(frozen=True, slots=True)
class BaseOrder(ABC):
    agent_id: str
    side: Side
    quantity: int

    order_id: int | None = field(default=None, kw_only=True)

    def __post_init__(self) -> None:
        if not self.agent_id:
            raise OrderError("agent_id must be a non-empty string")
        if not isinstance(self.quantity, int) or self.quantity <= 0:
            raise OrderError(f"quantity must be a positive int, got {self.quantity!r}")
        self._validate_type()

    @property
    @abstractmethod
    def order_type(self) -> OrderType:
        """Discriminator used by the engine for dispatch and events."""

    def _assign_id(self, order_id: int) -> None:
        # Engine-tagged id via slot write: a dataclasses.replace copy per
        # submission cost ~30% of runtime at bench scale.
        object.__setattr__(self, "order_id", order_id)

    def _validate_type(self) -> None:
        """Hook for subclass-specific field validation."""


@dataclass(frozen=True, slots=True)
class MarketOrder(BaseOrder):
    @property
    def order_type(self) -> OrderType:
        return OrderType.MARKET


@dataclass(frozen=True, slots=True)
class LimitOrder(BaseOrder):
    limit_price: float

    @property
    def order_type(self) -> OrderType:
        return OrderType.LIMIT

    @property
    def limit_ticks(self) -> int:
        return to_ticks(self.limit_price)

    def _validate_type(self) -> None:
        validate_price("limit_price", self.limit_price)


@dataclass(frozen=True, slots=True)
class MarketLimitOrder(BaseOrder):
    limit_price: float

    @property
    def order_type(self) -> OrderType:
        return OrderType.MARKET_LIMIT

    @property
    def limit_ticks(self) -> int:
        return to_ticks(self.limit_price)

    def _validate_type(self) -> None:
        validate_price("limit_price", self.limit_price)


@dataclass(frozen=True, slots=True)
class StopOrder(BaseOrder):
    stop_price: float

    @property
    def order_type(self) -> OrderType:
        return OrderType.STOP

    @property
    def stop_ticks(self) -> int:
        return to_ticks(self.stop_price)

    def _validate_type(self) -> None:
        validate_price("stop_price", self.stop_price)


@dataclass(frozen=True, slots=True)
class StopLimitOrder(BaseOrder):
    stop_price: float
    limit_price: float

    @property
    def order_type(self) -> OrderType:
        return OrderType.STOP_LIMIT

    @property
    def stop_ticks(self) -> int:
        return to_ticks(self.stop_price)

    @property
    def limit_ticks(self) -> int:
        return to_ticks(self.limit_price)

    def _validate_type(self) -> None:
        validate_price("stop_price", self.stop_price)
        validate_price("limit_price", self.limit_price)
