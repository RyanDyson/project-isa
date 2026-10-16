"""Market pipeline: engine events -> Redis mirror -> PostgreSQL journal.

Zero-latency contract: record() appends envelopes synchronously (memory
only); background tasks drain to Redis (pipelined) and flush to Postgres in
batches, acking stream entries only after the journal commit. Every piece of
Redis state is rebuildable; event_log is the source of truth.

Redis key layout: app/core/lob/redis_schema.md
"""

import asyncio
import json
import os
import uuid
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

from redis import asyncio as aioredis
from sqlalchemy.dialects.postgresql import insert as pg_insert

from app.core.lob.contracts import OrderType, Side
from app.core.lob.events import (
    Accepted,
    Cancelled,
    EngineEvent,
    Modified,
    StopTriggered,
    Trade,
)
from app.core.lob.registry import SymbolRegistry

RUN_ID = os.getenv("RUN_ID", f"dev-{uuid.uuid4().hex[:8]}")

_DRAIN_MAX_EVENTS = 512
_DRAIN_SECONDS = 0.005
_FLUSH_SECONDS = 30.0
_STREAM_BATCH = 2000


@dataclass(slots=True)
class _Envelope:
    symbol: str
    seq: int
    event_type: str
    payload: dict[str, Any]
    ts: datetime


def _utcnow() -> datetime:
    return datetime.now(UTC)


def price_to_ticks(price: float) -> int:
    return int(round(price * 100))


def _order_payload(order, sequence: int) -> dict[str, Any]:
    return {
        "order_id": order.order_id,
        "agent_id": order.agent_id,
        "side": order.side.value,
        "type": order.order_type.value,
        "quantity": order.quantity,
        "limit_price": getattr(order, "limit_price", None),
        "stop_price": getattr(order, "stop_price", None),
    }


def _state_of(record) -> dict[str, Any]:
    node = record.node
    limit_ticks = None
    stop_ticks = None
    if node.order_type in (OrderType.LIMIT, OrderType.MARKET_LIMIT):
        limit_ticks = node.limit
    if node.order_type in (OrderType.STOP, OrderType.STOP_LIMIT):
        stop_ticks = node.limit
        if node.order_type is OrderType.STOP_LIMIT:
            limit_ticks = node.conversion_limit
    return {
        "agent_id": node.agent_id,
        "side": node.buy_or_sell.value,
        "type": node.order_type.value,
        "quantity": record.order.quantity,
        "remaining": node.shares,
        "limit_ticks": limit_ticks,
        "stop_ticks": stop_ticks,
        "status": record.status.value,
        "seq": record.sequence,
    }


class MarketPipeline:
    """Owns the ring buffer, the Redis mirror, and the PG flusher."""

    def __init__(self, registry: SymbolRegistry) -> None:
        self.run_id = RUN_ID
        self._registry = registry
        self._redis: aioredis.Redis | None = None
        self._prefix = f"lob:{self.run_id}"
        self._queue: list[_Envelope] = []
        self._queue_limit = 100_000
        self._skipped = 0
        self._known_symbols: set[str] = set()
        self._drain_task: asyncio.Task | None = None
        self._flush_task: asyncio.Task | None = None
        self._closing = False
        self._started_at: datetime | None = None

    # -- lifecycle --------------------------------------------------------------

    async def start(self) -> None:
        self._redis = aioredis.from_url(
            os.getenv("REDIS_URL", "redis://localhost:6379/0"),
            decode_responses=True,
        )
        self._started_at = _utcnow()
        await self._redis.hset(
            f"{self._prefix}:meta",
            mapping={
                "run_id": self.run_id,
                "started_at": self._started_at.isoformat(),
                "mode": "manual",
            },
        )
        # Marker the C++ engine polls to discover the active run; its TTL is
        # refreshed by every drain batch, expiring once the backend dies.
        await self._redis.set("lob:active-run", self.run_id, ex=60)
        self._drain_task = asyncio.create_task(self._drain_loop())
        self._flush_task = asyncio.create_task(self._flush_loop())

    async def aclose(self) -> None:
        if self._closing:
            return
        self._closing = True
        for task in (self._drain_task, self._flush_task):
            if task is not None:
                task.cancel()
        drained = await self._drain_batch()
        counts = await self.flush_now(stamp_end=True)
        if self._redis is not None:
            await self._redis.hset(
                f"{self._prefix}:meta", mapping={"ended_at": _utcnow().isoformat()}
            )
            await self._redis.aclose()
            self._redis = None
        print(
            f"[pipeline] shutdown: drained {drained}, flushed "
            f"events={counts['events']} orders={counts['orders']} "
            f"trades={counts['trades']}"
        )

    # -- ingestion (called synchronously by the API layer) -----------------------

    def record(self, symbol: str, engine, events: list[EngineEvent]) -> int:
        queue = self._queue
        key = self._registry.normalize(symbol)
        self._registry.book(key)
        recorded = 0
        for event in events:
            payload = self._build_payload(engine, event)
            if payload is None:
                continue  # Rejected events are transport-level, not journaled
            if len(queue) >= self._queue_limit:
                self._skipped += 1
                continue
            queue.append(
                _Envelope(
                    symbol=key,
                    seq=payload["seq"],
                    event_type=payload["event"],
                    payload=payload,
                    ts=_utcnow(),
                )
            )
            recorded += 1
        return recorded

    def _build_payload(self, engine, event: EngineEvent) -> dict[str, Any] | None:
        affected: dict[int, dict[str, Any]] = {}

        if isinstance(event, Trade):
            for order_id in (event.buy_order_id, event.sell_order_id):
                record = engine._records.get(order_id)
                if record is not None:
                    affected[order_id] = _state_of(record)
            return {
                "event": "trade",
                "seq": event.sequence,
                "buy_order_id": event.buy_order_id,
                "sell_order_id": event.sell_order_id,
                "buy_agent": event.buy_agent,
                "sell_agent": event.sell_agent,
                "price": event.price,
                "quantity": event.quantity,
                "aggressor_side": event.aggressor_side.value,
                "affected": affected,
            }

        if isinstance(event, Accepted):
            record = engine._records[event.order.order_id]
            return {
                "event": "accepted",
                "seq": event.sequence,
                "order": _order_payload(event.order, record.sequence),
                "affected": {event.order.order_id: _state_of(record)},
            }

        if isinstance(event, Modified):
            record = engine._records[event.order.order_id]
            return {
                "event": "modified",
                "seq": event.sequence,
                "order": _order_payload(event.order, record.sequence),
                "affected": {event.order.order_id: _state_of(record)},
            }

        if isinstance(event, Cancelled):
            affected[event.order_id] = _state_of(engine._records[event.order_id])
            return {
                "event": "cancelled",
                "seq": event.sequence,
                "order_id": event.order_id,
                "affected": affected,
            }

        if isinstance(event, StopTriggered):
            record = engine._records[event.order_id]
            return {
                "event": "stop-triggered",
                "seq": event.sequence,
                "order_id": event.order_id,
                "converted": _order_payload(event.converted, record.sequence),
                "affected": {event.order_id: _state_of(record)},
            }

        return None

    # -- background drain: ring buffer -> Redis mirror ----------------------------

    async def _drain_loop(self) -> None:
        while self._closing is False:
            await asyncio.sleep(_DRAIN_SECONDS)
            try:
                await self._drain_batch()
            except Exception as error:  # noqa: BLE001 - keep the loop alive
                print(f"[pipeline] redis drain error, retrying: {error!r}")
                await asyncio.sleep(0.5)

    async def _drain_batch(self) -> int:
        if self._redis is None or not self._queue:
            return 0
        batch = self._queue[:_DRAIN_MAX_EVENTS]
        del self._queue[:_DRAIN_MAX_EVENTS]

        pipe = self._redis.pipeline(transaction=False)
        pipe.set("lob:active-run", self.run_id, ex=60)  # refresh the engine marker
        grouped: dict[str, list[_Envelope]] = {}
        for envelope in batch:
            grouped.setdefault(envelope.symbol, []).append(envelope)

        for symbol, envelopes in grouped.items():
            self._known_symbols.add(symbol)
            for envelope in envelopes:
                pipe.xadd(
                    f"{self._prefix}:{symbol}:events",
                    {
                        "seq": str(envelope.seq),
                        "type": envelope.event_type,
                        "ts": envelope.ts.isoformat(),
                        "payload": json.dumps(envelope.payload),
                    },
                )
                self._redis_ops_for_event(pipe, symbol, envelope)
        await pipe.execute()
        return len(batch)

    def _redis_ops_for_event(self, pipe, symbol: str, envelope: _Envelope) -> None:
        payload = envelope.payload
        event_type = envelope.event_type
        affected = payload.get("affected", {})

        if event_type == "trade":
            price_ticks = price_to_ticks(payload["price"])
            pipe.xadd(
                f"{self._prefix}:{symbol}:trades",
                {
                    "seq": payload["seq"],
                    "buy_order_id": payload["buy_order_id"],
                    "sell_order_id": payload["sell_order_id"],
                    "buy_agent": payload["buy_agent"],
                    "sell_agent": payload["sell_agent"],
                    "price_ticks": price_ticks,
                    "quantity": payload["quantity"],
                    "aggressor_side": payload["aggressor_side"],
                    "ts": envelope.ts.isoformat(),
                },
                maxlen=10_000,
                approximate=True,
            )
            pipe.set(f"{self._prefix}:{symbol}:last", price_ticks)

        for order_id, state in affected.items():
            order_hash = f"{self._prefix}:{symbol}:order:{order_id}"
            agent_key = f"{self._prefix}:agent:{state['agent_id']}:orders"
            member = f"{symbol}:{order_id}"
            if state["status"] in ("cancelled", "filled"):
                pipe.delete(order_hash)
                pipe.zrem(agent_key, member)
            else:
                pipe.hset(
                    order_hash,
                    mapping={
                        "agent": state["agent_id"],
                        "side": state["side"],
                        "type": state["type"],
                        "quantity": state["quantity"],
                        "remaining": state["remaining"],
                        "limit_ticks": state["limit_ticks"] or "",
                        "stop_ticks": state["stop_ticks"] or "",
                        "status": state["status"],
                        "seq": state["seq"],
                    },
                )
                pipe.zadd(agent_key, {member: state["seq"]})

        # Ladder mirror: snapshot rebuild for the touched symbol. Differential
        # updates later; at dev scale a full ladder rebuild per batch is cheap.
        book = self._registry.book(symbol)._book
        for side, ladder_key in (
            (Side.BUY, f"{self._prefix}:{symbol}:bids"),
            (Side.SELL, f"{self._prefix}:{symbol}:asks"),
        ):
            pipe.delete(ladder_key)
            ladder = book.bids if side is Side.BUY else book.asks
            for price_ticks, level in ladder.items():
                pipe.zadd(ladder_key, {str(price_ticks): price_ticks})
                pipe.hset(
                    f"{self._prefix}:{symbol}:level:{side.value}:{price_ticks}",
                    mapping={"size": level.size, "volume": level.total_volume},
                )

    # -- background flush: Redis streams -> PostgreSQL ----------------------------

    async def _flush_loop(self) -> None:
        while self._closing is False:
            await asyncio.sleep(_FLUSH_SECONDS)
            try:
                await self.flush_now()
            except Exception as error:  # noqa: BLE001 - keep the loop alive
                print(f"[pipeline] pg flush error, retrying: {error!r}")
                await asyncio.sleep(1.0)

    async def flush_now(self, *, stamp_end: bool = False) -> dict[str, int]:
        """Batch journal/projection flush; stream acks follow the commit."""
        from app.db import SessionMaker
        from app.db.models import EventLog, Order, Run, Trade

        if self._redis is None:
            return {"events": 0, "orders": 0, "trades": 0}

        journal_rows: list[dict[str, Any]] = []
        trade_rows: list[dict[str, Any]] = []
        order_states: dict[tuple[str, int], dict[str, Any]] = {}
        acks: dict[str, list[str]] = {}
        group = "pg-flusher"

        for symbol in sorted(self._known_symbols):
            stream = f"{self._prefix}:{symbol}:events"
            try:
                await self._redis.xgroup_create(stream, group, mkstream=True)
            except aioredis.ResponseError as error:
                if "BUSYGROUP" not in str(error):
                    raise

            response = await self._redis.xreadgroup(
                group,
                self.run_id,
                {stream: ">"},
                count=_STREAM_BATCH,
            )
            for _, entries in response:
                acks.setdefault(stream, [])
                for entry_id, fields in entries:
                    acks[stream].append(entry_id)
                    payload = json.loads(fields["payload"])
                    event_ts = datetime.fromisoformat(fields["ts"])
                    journal_rows.append(
                        {
                            "run_id": self.run_id,
                            "seq": int(fields["seq"]),
                            "symbol": symbol,
                            "ts": event_ts,
                            "event_type": fields["type"],
                            "payload": payload,
                        }
                    )
                    if fields["type"] == "trade":
                        trade_rows.append(
                            {
                                "run_id": self.run_id,
                                "seq": payload["seq"],
                                "symbol": symbol,
                                "buy_order_id": payload["buy_order_id"],
                                "sell_order_id": payload["sell_order_id"],
                                "buy_agent": payload["buy_agent"],
                                "sell_agent": payload["sell_agent"],
                                "price_ticks": price_to_ticks(payload["price"]),
                                "quantity": payload["quantity"],
                                "aggressor_side": payload["aggressor_side"],
                                "ts": event_ts,
                            }
                        )
                    for order_id, state in payload.get("affected", {}).items():
                        previous = order_states.get((symbol, order_id))
                        if previous is None or int(fields["seq"]) >= previous["seq"]:
                            order_states[(symbol, order_id)] = {
                                "state": state,
                                "seq": int(fields["seq"]),
                                "ts": event_ts,
                            }

        counts = {"events": 0, "orders": 0, "trades": 0}
        if not journal_rows:
            return counts

        async with SessionMaker() as session:
            await session.execute(
                pg_insert(Run)
                .values(
                    run_id=self.run_id,
                    started_at=self._started_at or _utcnow(),
                    mode="manual",
                )
                .on_conflict_do_nothing(index_elements=["run_id"])
            )
            await session.execute(
                pg_insert(EventLog)
                .values(journal_rows)
                .on_conflict_do_nothing(index_elements=["run_id", "seq"])
            )
            counts_events = len(journal_rows)

            counts_orders = 0
            now = _utcnow()
            for (symbol, order_id), order_state in order_states.items():
                state = order_state["state"]
                stmt = (
                    pg_insert(Order)
                    .values(
                        run_id=self.run_id,
                        order_id=order_id,
                        symbol=symbol,
                        agent_id=state["agent_id"],
                        side=state["side"],
                        order_type=state["type"],
                        quantity=state["quantity"],
                        limit_ticks=state["limit_ticks"],
                        stop_ticks=state["stop_ticks"],
                        status=state["status"],
                        created_seq=order_state["seq"],
                        created_at=order_state["ts"],
                        updated_seq=order_state["seq"],
                        updated_at=now,
                    )
                    .on_conflict_do_update(
                        index_elements=["run_id", "order_id"],
                        set_={
                            "status": stmt.excluded.status,
                            "updated_seq": stmt.excluded.updated_seq,
                            "updated_at": stmt.excluded.updated_at,
                        },
                    )
                )
                await session.execute(stmt)
                counts_orders += 1

            counts_trades = 0
            if trade_rows:
                await session.execute(
                    pg_insert(Trade)
                    .values(trade_rows)
                    .on_conflict_do_nothing(index_elements=["run_id", "seq"])
                )
                counts_trades = len(trade_rows)

            if stamp_end:
                await session.execute(
                    pg_insert(Run)
                    .values(
                        run_id=self.run_id,
                        started_at=self._started_at or _utcnow(),
                        ended_at=_utcnow(),
                        mode="manual",
                    )
                    .on_conflict_do_update(
                        index_elements=["run_id"],
                        set_={"ended_at": _utcnow()},
                    )
                )
            await session.commit()

        for stream, ids in acks.items():
            await self._redis.xack(stream, group, *ids)

        counts = {
            "events": counts_events,
            "orders": counts_orders,
            "trades": counts_trades,
        }
        print(
            f"[pipeline] flushed events={counts['events']} "
            f"orders={counts['orders']} trades={counts['trades']}"
        )
        return counts
