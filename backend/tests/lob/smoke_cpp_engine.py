"""Integration smoke for the C++ matching service, run against live Redis +
the lob-engine container + FastAPI.

Requirements: docker compose up -d redis lob-engine, then start the backend
(uvicorn app.main:app). This script drives the engine_client (the same
network path the REST routes use) and cross-checks the hot Redis layer.

Run:  cd backend && python -m tests.lob.smoke_cpp_engine
"""

import json
import os
import time

from app.storage.engine_client import LobEngineClient
from app.storage.pipeline import RUN_ID

REDIS_URL = os.getenv("REDIS_URL", "redis://localhost:6379/0")

failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if condition else 'FAIL'}  {label}{'' if condition else f'  ({detail})'}")
    if not condition:
        failures.append(label)


def main() -> None:
    client = LobEngineClient(REDIS_URL, RUN_ID)
    sym = "SMOKE"

    try:
        # The engine drives only when the run marker is live; refresh before
        # the first command (the FastAPI lifespan does this in production).
        client.refresh_run_marker()
        time.sleep(2.0)

        # resting liquidity: sell 10 @ 101 (a1), buy 5 @ 100 (a2)
        reply = client.call(
            {
                "action": "submit",
                "symbol": sym,
                "agent_id": "a1",
                "type": "limit",
                "side": "sell",
                "quantity": 10,
                "limit_price": 101.0,
                "client_order_ref": "sm1",
            }
        )
        check("limit sell accepted", reply["status"] == "ok")
        print("[smoke] reply:", json.dumps(reply, default=str)[:400])
        sell_id = reply["events"][0]["order"]["order_id"]
        reply = client.call(
            {
                "action": "submit",
                "symbol": sym,
                "agent_id": "a2",
                "type": "limit",
                "side": "buy",
                "quantity": 5,
                "limit_price": 100.0,
            }
        )
        buy_id = reply["events"][0]["order"]["order_id"]

        # aggressive market-limit buy 7 @ 101.5: taker phase trades 7 @ 101,
        # remainder rests at 101.5
        reply = client.call(
            {
                "action": "submit",
                "symbol": sym,
                "agent_id": "a2",
                "type": "market-limit",
                "side": "buy",
                "quantity": 7,
                "limit_price": 101.5,
            }
        )
        trades = [e for e in reply["events"] if e["event"] == "trade"]
        check("market-limit produces 1 trade", len(trades) == 1)
        check(
            "trade prints maker price 101 @ qty 7",
            trades and trades[0]["price"] == 101.0 and trades[0]["quantity"] == 7,
        )
        check("aggressor is buy", trades and trades[0]["aggressor_side"] == "buy")

        # pure limit that would cross must be rejected outright
        reply = client.call(
            {
                "action": "submit",
                "symbol": sym,
                "agent_id": "a2",
                "type": "limit",
                "side": "buy",
                "quantity": 1,
                "limit_price": 101.0,
            }
        )
        rejected = [e for e in reply["events"] if e["event"] == "rejected"]
        check("crossing pure limit rejected", len(rejected) == 1)

        # dormant sell stop @ 95 (last now 101) fires after a fed price 94
        client.call(
            {
                "action": "submit",
                "symbol": sym,
                "agent_id": "a1",
                "type": "stop",
                "side": "sell",
                "quantity": 4,
                "stop_price": 95.0,
            }
        )
        reply = client.call({"action": "price", "symbol": sym, "price": 94.0})
        kinds = [e["event"] for e in reply["events"]]
        check("price feed triggers the stop", "stop-triggered" in kinds)
        check("converted stop trades the resting bid", "trade" in kinds)
        trade = next(e for e in reply["events"] if e["event"] == "trade")
        check(
            "stop conversion trades 4 @ 100 aggressor sell",
            trade["quantity"] == 4 and trade["price"] == 100.0 and trade["aggressor_side"] == "sell",
        )

        # cancel by a non-owner must be refused
        reply = client.call(
            {"action": "cancel", "symbol": sym, "agent_id": "a9", "order_id": sell_id}
        )
        rejected = [e for e in reply["events"] if e["event"] == "rejected"]
        check("cancel by non-owner rejected", len(rejected) == 1)

        # ownership ok
        reply = client.call(
            {"action": "cancel", "symbol": sym, "agent_id": "a1", "order_id": sell_id}
        )
        check("cancel by owner ok", reply["events"][0]["event"] == "cancelled")

        # mirrored hot layer: tape + last; the fed price tripped the stop,
        # whose own trade printed 100 -> `last` ends there (trade price wins)
        tape = client._redis.xrevrange(f"lob:{RUN_ID}:{sym}:trades", count=10)
        check("trade tape has entries", len(tape) >= 2)
        last = client._redis.get(f"lob:{RUN_ID}:{sym}:last")
        last_value = float(last.decode() if isinstance(last, bytes) else last) if last else None
        check("last price mirrored", last_value == 10000, f"got {last_value}")
    finally:
        client.close()

    if failures:
        print(f"\n{len(failures)} FAILED smoke checks")
        raise SystemExit(1)
    print("\nsmoke passed")


if __name__ == "__main__":
    main()
