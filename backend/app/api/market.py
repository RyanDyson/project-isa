"""REST endpoints exposing the LOB engine(s) to the dashboard.

Route segment ``{symbol}`` routes to that ticker's own single-writer book
(see app/core/lob/registry.py). Module-level registry singleton: FastAPI must
run with a single worker. Auth linkage (agent_id -> session user) lands later.
"""

from collections import deque

from fastapi import APIRouter, Body, HTTPException, Path
from pydantic import BaseModel

from app.core.lob import (
    LimitOrder,
    SymbolRegistry,
    MarketOrder,
    MarketLimitOrder,
    OrderError,
    Rejected,
    Side,
    StopLimitOrder,
    StopOrder,
    StopTriggered,
    from_ticks,
)
from app.core.lob.engine import LimitOrderBook
from app.core.lob.events import Accepted, Cancelled, EngineEvent, Modified, Trade
from app.storage.pipeline import MarketPipeline

router = APIRouter(prefix="/api/market", tags=["market"])

registry = SymbolRegistry()
pipeline = MarketPipeline(registry)
recent_trades: dict[str, deque[dict]] = {}


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


_SYMBOL_PATH = Path(..., min_length=1, max_length=16)


def _trade_buffer(symbol: str) -> deque[dict]:
    return recent_trades.setdefault(symbol, deque(maxlen=200))


def _build_order(body: SubmitOrderBody):
    kwargs = dict(agent_id=body.agent_id, side=body.side, quantity=body.quantity)
    if body.type == "market":
        return MarketOrder(**kwargs)
    if body.type == "limit":
        if body.limit_price is None:
            raise HTTPException(status_code=422, detail="limit_price is required for limit orders")
        return LimitOrder(limit_price=body.limit_price, **kwargs)
    if body.type == "market-limit":
        if body.limit_price is None:
            raise HTTPException(
                status_code=422, detail="limit_price is required for market-limit orders"
            )
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
        return {
            "event": "modified",
            "order_id": event.order.order_id,
            "order": _order_as_dict(event.order),
        }
    if isinstance(event, Cancelled):
        return {"event": "cancelled", "order_id": event.order_id}
    if isinstance(event, StopTriggered):
        return {
            "event": "stop-triggered",
            "order_id": event.order_id,
            "converted": _order_as_dict(event.converted),
        }
    raise TypeError(f"unmapped event {type(event)}")


def _collect_trades(symbol: str, events: list[EngineEvent]) -> None:
    for event in events:
        if isinstance(event, Trade):
            _trade_buffer(symbol).append(_event_as_dict(event))


def _depth(engine: LimitOrderBook, side: Side, depth: int) -> list[dict]:
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


# -- symbols ----------------------------------------------------------------------


@router.get("/run")
def current_run() -> dict:
    return {"run_id": pipeline.run_id}


@router.post("/flush")
async def flush_run() -> dict:
    return await pipeline.flush_now()


@router.get("/symbols")
def list_symbols() -> dict:
    return {"symbols": registry.symbols()}


# -- submissions ------------------------------------------------------------------


@router.post("/{symbol}/orders")
def submit_order(
    symbol: str = _SYMBOL_PATH,
    body: SubmitOrderBody = Body(...),
) -> dict:
    order = _build_order(body)
    engine = registry.book(symbol)
    try:
        events = engine.submit(order)
        pipeline.record(symbol, engine, events)
    except OrderError as error:
        raise HTTPException(status_code=400, detail=str(error))
    _collect_trades(symbol, events)
    accepted = next((e for e in events if isinstance(e, Accepted)), None)
    return {
        "symbol": symbol.upper(),
        "order_id": accepted.order.order_id if accepted else None,
        "events": [_event_as_dict(event) for event in events],
    }


@router.post("/{symbol}/orders/{order_id}/cancel")
def cancel_order(
    order_id: int,
    body: CancelBody,
    symbol: str = _SYMBOL_PATH,
) -> dict:
    engine = registry.book(symbol)
    outcome = engine.cancel(order_id=order_id, agent_id=body.agent_id)
    pipeline.record(symbol, engine, [outcome])
    return {"outcome": _event_as_dict(outcome)}


@router.patch("/{symbol}/orders/{order_id}")
def modify_order(
    order_id: int,
    body: ModifyOrderBody,
    symbol: str = _SYMBOL_PATH,
) -> dict:
    engine = registry.book(symbol)
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
    pipeline.record(symbol, engine, [outcome])
    return {"outcome": _event_as_dict(outcome)}


@router.post("/{symbol}/price")
def feed_price(
    symbol: str = _SYMBOL_PATH,
    body: PriceFeedBody = Body(...),
) -> dict:
    """Dev convenience: feeds the last traded price (standing in for the
    ingestion pipeline until it exists)."""
    engine = registry.book(symbol)
    try:
        events = engine.update_market_price(body.price)
    except OrderError as error:
        raise HTTPException(status_code=400, detail=str(error))
    pipeline.record(symbol, engine, events)
    _collect_trades(symbol, events)
    return {
        "symbol": symbol.upper(),
        "last_trade_price": engine.last_trade_price,
        "events": [_event_as_dict(event) for event in events],
    }


# -- reads --------------------------------------------------------------------------


@router.get("/{symbol}/book")
def get_book(symbol: str = _SYMBOL_PATH, depth: int = 10) -> dict:
    engine = registry.book(symbol)
    best_bid, best_ask = engine.get_best_bid_ask()
    return {
        "symbol": symbol.upper(),
        "best_bid": best_bid,
        "best_ask": best_ask,
        "last_trade_price": engine.last_trade_price,
        "bids": _depth(engine, Side.BUY, depth),
        "asks": _depth(engine, Side.SELL, depth),
    }


@router.get("/{symbol}/trades")
def get_trades(symbol: str = _SYMBOL_PATH, limit: int = 50) -> list[dict]:
    buffer = _trade_buffer(symbol)
    return list(buffer)[-limit:]
