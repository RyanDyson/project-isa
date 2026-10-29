"""Market pipeline: C++ engine events -> PostgreSQL journal.

Since matching moved into the C++ microservice (../lob_engine), the python
side owns only the run lifecycle marker and the Postgres flush:
 - ``start()`` writes the run meta hash and the ``lob:active-run`` marker the
   engine polls; the flush loop refreshes its TTL (the engine detaches when
   the backend dies).
 - ``flush_now()`` XREADGROUPs the engine-journaled event streams (group
   ``pg-flusher``), upserts journal / order / trade rows, and acks stream
   entries only after the transaction commits.
 - Active symbols come from the ``lob:{run}:symbols`` SET the engine
   maintains (reads happen sync through LobEngineClient in market.py).

The hot-layer mirrors (ladder zsets, order hashes, agent index, trade tape,
last price) are written inside the C++ service itself; see
app/storage/redis_schema.md.
"""

import asyncio
import json
import os
import uuid
from datetime import UTC, datetime
from typing import Any

from redis import asyncio as aioredis
from sqlalchemy.dialects.postgresql import insert as pg_insert

RUN_ID = os.getenv("RUN_ID", f"dev-{uuid.uuid4().hex[:8]}")

_FLUSH_SECONDS = 30.0
_STREAM_BATCH = 2000


def _utcnow() -> datetime:
    return datetime.now(UTC)


def price_to_ticks(price: float) -> int:
    return int(round(price * 100))


class MarketPipeline:
    """Owns the run lifecycle marker and the Postgres flusher."""

    def __init__(self) -> None:
        self.run_id = RUN_ID
        self._redis: aioredis.Redis | None = None
        self._flush_task: asyncio.Task | None = None
        self._closing = False
        self._started_at: datetime | None = None

    # -- lifecycle ---------------------------------------------------------------

    async def start(self) -> None:
        self._redis = aioredis.from_url(
            os.getenv("REDIS_URL", "redis://localhost:6379/0"),
            decode_responses=True,
        )
        self._started_at = _utcnow()
        await self._redis.hset(
            f"lob:{self.run_id}:meta",
            mapping={
                "run_id": self.run_id,
                "started_at": self._started_at.isoformat(),
                "mode": "manual",
            },
        )
        # Marker the C++ engine polls to discover the active run; TTL 60 s so
        # the engine detaches once the backend dies.
        await self._redis.set("lob:active-run", self.run_id, ex=60)
        self._flush_task = asyncio.create_task(self._flush_loop())

    async def aclose(self) -> None:
        if self._closing:
            return
        self._closing = True
        if self._flush_task is not None:
            self._flush_task.cancel()
        counts = await self.flush_now(stamp_end=True)
        if self._redis is not None:
            await self._redis.hset(
                f"lob:{self.run_id}:meta", mapping={"ended_at": _utcnow().isoformat()}
            )
            await self._redis.aclose()
            self._redis = None
        print(
            f"[pipeline] shutdown: flushed "
            f"events={counts['events']} orders={counts['orders']} trades={counts['trades']}"
        )

    # -- background flush: engine journal -> PostgreSQL ---------------------------

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

        redis = self._redis
        if redis is None:
            return {"events": 0, "orders": 0, "trades": 0}

        symbols = sorted(await redis.smembers(f"lob:{self.run_id}:symbols"))
        await redis.set("lob:active-run", self.run_id, ex=60)  # refresh the engine marker

        journal_rows: list[dict[str, Any]] = []
        trade_rows: list[dict[str, Any]] = []
        order_states: dict[tuple[str, int], dict[str, Any]] = {}
        acks: dict[str, list[str]] = {}
        group = "pg-flusher"

        for symbol in symbols:
            stream = f"lob:{self.run_id}:{symbol}:events"
            try:
                await redis.xgroup_create(stream, group, mkstream=True)
            except aioredis.ResponseError as error:
                if "BUSYGROUP" not in str(error):
                    raise

            response = await redis.xreadgroup(
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
            counts["events"] = len(journal_rows)

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
                counts["orders"] += 1

            if trade_rows:
                await session.execute(
                    pg_insert(Trade)
                    .values(trade_rows)
                    .on_conflict_do_nothing(index_elements=["run_id", "seq"])
                )
                counts["trades"] = len(trade_rows)

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
            await redis.xack(stream, group, *ids)

        print(
            f"[pipeline] flushed events={counts['events']} "
            f"orders={counts['orders']} trades={counts['trades']}"
        )
        return counts


# Module-level singleton (mounted by app.api.market; driven from app.main's
# lifespan: start() on boot, aclose() on shutdown).
pipeline = MarketPipeline()
