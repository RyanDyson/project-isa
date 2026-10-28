"""REST endpoints exposing the C++ matching service to the dashboard.

Matching moved into the C++ microservice (../lob_engine): these routes
translate REST into engine commands (Redis Streams command/reply protocol,
see app/storage/engine_client.py) and serve reads straight from the hot
Redis layer the engine mirrors after every command. Symbol normalization
mirrors the old SymbolRegistry: uppercase, whitespace trimmed, max length 16.
"""

import os

from fastapi import APIRouter, Body, HTTPException, Path
from pydantic import BaseModel
from redis import Redis

from app.storage.engine_client import EngineTimeoutError, LobEngineClient
from app.storage.pipeline import RUN_ID, pipeline

router = APIRouter(prefix="/api/market", tags=["market"])

PRICE_TICK = 0.01

_client = LobEngineClient(os.getenv("REDIS_URL", "redis://localhost:6379/0"), RUN_ID)
_hot = Redis.from_url(os.getenv("REDIS_URL", "redis://localhost:6379/0"), decode_responses=False)


class SubmitOrderBody(BaseModel):
    agent_id: str
    type: str  # market | limit | market-limit | stop | stop-limit
    side: str  # buy | sell
    quantity: int
    limit_price: float | None = None
    stop_price: float | None = None
    client_order_ref: str | None = None


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


def _normalize(symbol: str) -> str:
    key = symbol.strip().upper()
    if not key:
        raise HTTPException(422, "symbol must not be empty")
    return key


def _validated_price(name: str, value: float) -> float:
    """Same construct-time contract the python engine enforced: positive and
    an exact multiple of the tick size."""
    if value <= 0:
        raise HTTPException(422, f"{name} must be positive")
    ticks = round(value / PRICE_TICK)
    if abs(ticks * PRICE_TICK - value) > 1e-9:
        raise HTTPException(422, f"{name} must be a multiple of the tick size {PRICE_TICK}")
    return value


def _call_engine(command: dict) -> dict:
    try:
        return _client.call(command)
    except EngineTimeoutError as error:
        raise HTTPException(503, "matching engine unavailable") from error


def _event_from_journal(payload: dict) -> dict:
    """Journal shapes (see storage/pipeline.py pg-flusher) -> REST shapes."""
    kind = payload["event"]
    if kind == "accepted":
        order = payload["order"]
        return {
            "event": "accepted",
            "sequence": payload["seq"],
            "order_id": order["order_id"],
            "order": order,
        }
    if kind == "trade":
        return dict(payload)
    if kind == "cancelled":
        return {"event": "cancelled", "order_id": payload["order_id"]}
    if kind == "modified":
        order = payload["order"]
        return {
            "event": "modified",
            "sequence": payload["seq"],
            "order_id": order["order_id"],
            "order": order,
        }
    if kind == "stop-triggered":
        return {
            "event": "stop-triggered",
            "order_id": payload["order_id"],
            "converted": payload["converted"],
        }
    if kind == "rejected":
        return {
            "event": "rejected",
            "order_id": payload.get("order_id"),
            "reason": payload.get("reason"),
        }
    raise HTTPException(502, f"unmapped journal event '{kind}'")


# -- symbols / lifecycle ----------------------------------------------------


@router.get("/run")
def current_run() -> dict:
    return {"run_id": RUN_ID}


@router.post("/flush")
async def flush_run() -> dict:
    return await pipeline.flush_now()


@router.get("/symbols")
def list_symbols() -> dict:
    return {"symbols": _client.active_symbols()}


# -- submissions -------------------------------------------------------------


@router.post("/{symbol}/orders")
def submit_order(symbol: str = _SYMBOL_PATH, body: SubmitOrderBody = Body(...)) -> dict:
    key = _normalize(symbol)
    if body.side not in ("buy", "sell"):
        raise HTTPException(422, f"unknown side '{body.side}'")
    command: dict = {
        "action": "submit",
        "symbol": key,
        "agent_id": body.agent_id,
        "type": body.type,
        "side": body.side,
        "quantity": body.quantity,
    }
    if body.limit_price is not None:
        command["limit_price"] = _validated_price("limit_price", body.limit_price)
    if body.stop_price is not None:
        command["stop_price"] = _validated_price("stop_price", body.stop_price)
    if body.client_order_ref:
        command["client_order_ref"] = body.client_order_ref

    reply = _call_engine(command)
    events = [_event_from_journal(event) for event in reply.get("events", [])]
    order_id = next(
        (event["order_id"] for event in events if event["event"] == "accepted"), None
    )
    return {
        "symbol": key,
        "order_id": order_id,
        "status": reply.get("status", "ok"),
        "events": events,
    }


@router.post("/{symbol}/orders/{order_id}/cancel")
def cancel_order(
    order_id: int,
    body: CancelBody,
    symbol: str = _SYMBOL_PATH,
) -> dict:
    key = _normalize(symbol)
    reply = _call_engine(
        {"action": "cancel", "symbol": key, "agent_id": body.agent_id, "order_id": order_id}
    )
    outcomes = [_event_from_journal(event) for event in reply.get("events", [])]
    return {
        "status": reply.get("status", "ok"),
        "outcome": outcomes[0] if outcomes else None,
    }


@router.patch("/{symbol}/orders/{order_id}")
def modify_order(
    order_id: int,
    body: ModifyOrderBody,
    symbol: str = _SYMBOL_PATH,
) -> dict:
    key = _normalize(symbol)
    if body.quantity is None and body.limit_price is None and body.stop_price is None:
        raise HTTPException(422, "no fields passed to modify")
    command: dict = {
        "action": "modify",
        "symbol": key,
        "agent_id": body.agent_id,
        "order_id": order_id,
    }
    if body.quantity is not None:
        command["quantity"] = body.quantity
    if body.limit_price is not None:
        command["limit_price"] = _validated_price("limit_price", body.limit_price)
    if body.stop_price is not None:
        command["stop_price"] = _validated_price("stop_price", body.stop_price)

    reply = _call_engine(command)
    outcomes = [_event_from_journal(event) for event in reply.get("events", [])]
    rejected = next(
        (outcome for outcome in outcomes if outcome["event"] == "rejected"), None
    )
    if rejected is not None:
        raise HTTPException(400, rejected.get("reason") or "order rejected")
    # e.g. a quantity-only patch emits Modified; an instantly-triggered stop
    # also emits stop-triggered + trades, all surfaced through events
    return {
        "status": reply.get("status", "ok"),
        "outcome": outcomes[0] if outcomes else None,
        "events": outcomes,
    }


@router.post("/{symbol}/price")
def feed_price(symbol: str = _SYMBOL_PATH, body: PriceFeedBody = Body(...)) -> dict:
    key = _normalize(symbol)
    last = _validated_price("price", body.price)
    reply = _call_engine({"action": "price", "symbol": key, "price": last})
    events = [_event_from_journal(event) for event in reply.get("events", [])]
    return {
        "symbol": key,
        "status": reply.get("status", "ok"),
        "last_trade_price": last,
        "events": events,
    }


# -- reads (hot Redis layer maintained by the engine) -------------------------


def _depth(key: str, ladder: str, depth: int, reverse: bool) -> list[dict]:
    index_key = f"lob:{RUN_ID}:{key}:{ladder}"
    level_side = "buy" if ladder == "bids" else "sell"
    pairs = _hot.zrevrange(index_key, 0, depth - 1) if reverse else _hot.zrange(
        index_key, 0, depth - 1
    )
    rows = []
    for raw_ticks in pairs:
        ticks = int(raw_ticks)
        size, volume = _hot.hmget(f"lob:{RUN_ID}:{key}:level:{level_side}:{ticks}",
                                  ["size", "volume"])
        rows.append(
            {
                "price": ticks * PRICE_TICK,
                "orders": int(size) if size else 0,
                "volume": int(volume) if volume else 0,
            }
        )
    return rows


@router.get("/{symbol}/book")
def get_book(symbol: str = _SYMBOL_PATH, depth: int = 10) -> dict:
    key = _normalize(symbol)
    best_bid = _hot.zrevrange(f"lob:{RUN_ID}:{key}:bids", 0, 0)
    best_ask = _hot.zrange(f"lob:{RUN_ID}:{key}:asks", 0, 0)
    raw_last = _hot.get(f"lob:{RUN_ID}:{key}:last")
    return {
        "symbol": key,
        "best_bid": int(best_bid[0]) * PRICE_TICK if best_bid else None,
        "best_ask": int(best_ask[0]) * PRICE_TICK if best_ask else None,
        "last_trade_price": int(raw_last) * PRICE_TICK if raw_last else None,
        "bids": _depth(key, "bids", depth, reverse=True),
        "asks": _depth(key, "asks", depth, reverse=False),
    }


@router.get("/{symbol}/trades")
def get_trades(symbol: str = _SYMBOL_PATH, limit: int = 50) -> list[dict]:
    key = _normalize(symbol)
    rows = []
    for _, fields in _hot.xrevrange(f"lob:{RUN_ID}:{key}:trades", count=limit):
        rows.append(
            {
                "sequence": int(fields[b"seq"]),
                "buy_order_id": int(fields[b"buy_order_id"]),
                "sell_order_id": int(fields[b"sell_order_id"]),
                "buy_agent": fields[b"buy_agent"].decode(),
                "sell_agent": fields[b"sell_agent"].decode(),
                "price": int(fields[b"price_ticks"]) * PRICE_TICK,
                "quantity": int(fields[b"quantity"]),
                "aggressor_side": fields[b"aggressor_side"].decode(),
            }
        )
    return rows
