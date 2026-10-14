"""REST endpoints exposing the LOB engine to the dashboard.

Module-level engine singleton: FastAPI must run with a single worker so the
single-writer book is only ever mutated by this process. Auth linkage
(agent_id -> session user) lands later.
"""

from collections import deque

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

from app.core.lob import (
    LimitOrder,
    LimitOrderBook,
    MarketOrder,
    MarketLimitOrder,
    OrderError,
    OrderRejectedError,
    Rejected,
    Side,
    StopLimitOrder,
    StopOrder,
    StopTriggered,
    from_ticks,
    to_ticks,
)
from app.core.lob.events import (
    Accepted,
    Cancelled,
    EngineEvent,
    Modified,
    Trade,
)

router = APIRouter(prefix="/api/market", tags=["market"])

engine = LimitOrderBook()
recent_trades: deque[dict] = deque(maxlen=200)


class SubmitOrderBody(BaseModel):
    agent_id: str
    type: str  # market | limit | market-limit | stop | stop-limit
    side: Side
    quantity: int
    limit_price: float | None = None
    stop_price: float | None = None


class CancelBody(BaseModel):
    agent_id: str


class ModifyOrderBody(BaseModel):
    agent_id: str
    quantity: int | None = None
    limit_price: float | None = None
    stop_price: float | None = None


class PriceFeedBody(BaseModel):
    price: float


def _build_order(body: SubmitOrderBody):
    kwargs = dict(
        agent_id=body.agent_id,
        side=body.side,
        quantity=body.quantity,
    )
    if body.type == "market":
        return MarketOrder(**kwargs)
    if body.type == "limit":
        if body.limit_price is None:
            raise HTTPException(status_code=422, detail="limit_price is required for limit orders")
        return LimitOrder(limit_price=body.limit_price, **kwargs)
    if body.type == "market-limit":
        if body.limit_price is None:
            raise HTTPException(status_code=422, detail="limit_price is required for market-limit orders")
        return MarketLimitOrder(limit_price=body.limit_price, **kwargs)
    if body.type == "stop":
        if body.stop_price is None:
            raise HTTPException(status_code=422, detail="stop_price is required for stop orders")
        return StopOrder(stop_price=body.stop_price, **kwargs)
    if body.type == "stop-limit":
        if body.stop_price is None or body.limit_price is None:
            raise HTTPException(
                status_code=422,
                detail="stop_limit orders require both stop_price and limit_price",
            )
        return StopLimitOrder(stop_price=body.stop_price, limit_price=body.limit_price, **kwargs)
    raise HTTPException(status_code=422, detail=f"unknown order type '{body.type}'")


def _order_as_dict(order) -> dict:
    return {
        "order_id": order.order_id,
        "agent_id": order.agent_id,
        "side": order.side.value,
        "type": order.order_type.value,
        "quantity": order.quantity,
        "limit_price": getattr(order, "limit_price", None),
        "stop_price": getattr(order, "stop_price", None),
    }


def _event_as_dict(event: EngineEvent) -> dict:
    if isinstance(event, Accepted):
        return {
            "event": "accepted",
            "order_id": event.order.order_id,
            "sequence": event.sequence,
            "order": _order_as_dict(event.order),
        }
    if isinstance(event, Trade):
        return {
            "event": "trade",
            "sequence": event.sequence,
            "buy_order_id": event.buy_order_id,
            "sell_order_id": event.sell_order_id,
            "buy_agent": event.buy_agent,
            "sell_agent": event.sell_agent,
            "price": event.price,
            "quantity": event.quantity,
            "aggressor_side": event.aggressor_side.value,
        }
    if isinstance(event, Rejected):
        return {
            "event": "rejected",
            "order_id": event.order.order_id if event.order else None,
            "reason": event.reason,
        }
    if isinstance(event, Modified):
        return {"event": "modified", "order_id": event.order.order_id, "order": _order_as_dict(event.order)}
    if isinstance(event, Cancelled):
        return {"event": "cancelled", "order_id": event.order_id}
    if isinstance(event, StopTriggered):
        return {
            "event": "stop-triggered",
            "order_id": event.order_id,
            "converted": _order_as_dict(event.converted),
        }
    raise TypeError(f"unmapped event {type(event)}")


def _collect_trades(events: list[EngineEvent]) -> None:
    for event in events:
        if isinstance(event, Trade):
            recent_trades.append(_event_as_dict(event))


def _depth(side: Side, depth: int) -> list[dict]:
    ladder = engine._book.asks if side is Side.SELL else engine._book.bids
    items = list(ladder.items())
    if side is Side.BUY:
        items = list(reversed(items))
    return [
        {
            "price": from_ticks(price_ticks),
            "orders": level.size,
            "volume": level.total_volume,
        }
        for price_ticks, level in items[:depth]
    ]


# -- submissions ---------------------------------------------------------------


@router.post("/orders")
def submit_order(body: SubmitOrderBody) -> dict:
    order = _build_order(body)
    try:
        events = engine.submit(order)
    except OrderError as error:
        raise HTTPException(status_code=400, detail=str(error))
    _collect_trades(events)
    accepted = next(
        (e for e in events if isinstance(e, Accepted)), None
    )
    return {
        "order_id": accepted.order.order_id if accepted else None,
        "events": [_event_as_dict(event) for event in events],
    }


@router.post("/orders/{order_id}/cancel")
def cancel_order(order_id: int, body: CancelBody) -> dict:
    outcome = engine.cancel(order_id=order_id, agent_id=body.agent_id)
    return {"outcome": _event_as_dict(outcome)}


@router.patch("/orders/{order_id}")
def modify_order(order_id: int, body: ModifyOrderBody) -> dict:
    try:
        outcome = engine.modify(
            order_id=order_id,
            agent_id=body.agent_id,
            quantity=body.quantity,
            limit_price=body.limit_price,
            stop_price=body.stop_price,
        )
    except OrderError as error:
        raise HTTPException(status_code=400, detail=str(error))
    return {"outcome": _event_as_dict(outcome)}


@router.post("/price")
def feed_price(body: PriceFeedBody) -> dict:
    """Dev convenience: feeds the last traded price (standing in for the
    ingestion pipeline until it exists)."""
    try:
        events = engine.update_market_price(body.price)
    except OrderError as error:
        raise HTTPException(status_code=400, detail=str(error))
    _collect_trades(events)
    return {
        "last_trade_price": engine.last_trade_price,
        "events": [_event_as_dict(event) for event in events],
    }


# -- reads ----------------------------------------------------------------------


@router.get("/book")
def get_book(depth: int = 10) -> dict:
    best_bid, best_ask = engine.get_best_bid_ask()
    return {
        "best_bid": best_bid,
        "best_ask": best_ask,
        "last_trade_price": engine.last_trade_price,
        "bids": _depth(Side.BUY, depth),
        "asks": _depth(Side.SELL, depth),
    }


@router.get("/trades")
def get_trades(limit: int = 50) -> list[dict]:
    return list(recent_trades)[-limit:]
