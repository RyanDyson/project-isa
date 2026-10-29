"""REST -> C++ engine bridge (Redis Streams command/reply protocol).

Matching lives in the C++ microservice (../lob_engine). Each call XADDs a
JSON command to ``lob:{run}:commands`` and polls ``lob:{run}:reply:{corr_id}``
(the engine TTLs replies at 8 s) until it appears. A coroutine-free sync
client on purpose: the REST routes run in FastAPI's threadpool.
"""

import json
import time
import uuid

from redis import Redis

_REPLY_POLL_SECONDS = 5.0
_REPLY_POLL_INTERVAL = 0.002


class EngineTimeoutError(RuntimeError):
    """The engine did not reply within the poll window."""


class LobEngineClient:
    """Tiny sync client: build command -> XADD -> await correlated reply."""

    def __init__(self, redis_url: str, run_id: str) -> None:
        self._run_id = run_id
        self._redis = Redis.from_url(redis_url, decode_responses=False)

    def call(self, payload: dict) -> dict:
        corr_id = uuid.uuid4().hex
        payload["corr_id"] = corr_id
        self._redis.xadd(f"lob:{self._run_id}:commands", {"payload": json.dumps(payload)})
        deadline = time.monotonic() + _REPLY_POLL_SECONDS
        reply_key = f"lob:{self._run_id}:reply:{corr_id}"
        while time.monotonic() < deadline:
            reply = self._redis.get(reply_key)
            if reply is not None:
                return json.loads(reply)
            time.sleep(_REPLY_POLL_INTERVAL)
        raise EngineTimeoutError(f"no engine reply for corr_id {corr_id}")

    def active_symbols(self) -> list[str]:
        """Tickers the engine has mirrored this run (see redis_schema.md)."""
        members = self._redis.smembers(f"lob:{self._run_id}:symbols")
        out = []
        for member in members:
            text = member.decode() if isinstance(member, bytes) else member
            out.append(text)
        return sorted(out)

    def refresh_run_marker(self, ttl_seconds: int = 60) -> None:
        """Make this run discoverable for the engine (REST cookies don't do
        this automatically when the FastAPI lifespan isn't driving the run)."""
        self._redis.set("lob:active-run", self._run_id, ex=ttl_seconds)

    def close(self) -> None:
        self._redis.close()
