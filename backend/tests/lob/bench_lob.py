"""Throughput benchmark for the AVL/DLL limit order book (app/core).

Machine-dependent numbers; single-threaded, single-writer, deterministic via
--seed. Phases:

  1. resting_adds   - limit submissions that never cross: pure tree + DLL cost
  2. cancels        - FIFO relinks incl. empty-level tree deletions
  3. modifies       - in-place working-size updates (priority retention)
  4. taker_sweeps   - preloaded ladder hammered by markets/crossing limits
                      (the real matching-loop cost: fills/sec)
  5. stop_storm     - pending stops arming both sides of the touch, then a
                      price walk that fires them (tree pops + sweeps)
  6. mixed_pipeline - agent-like churn: adds / cancels / modifies /
                      market-limits in a realistic ratio

Each phase gets a fresh engine; invariant checks run once per phase when
--invariants is passed. The engine here has no Redis/PG flushers: these are
the in-memory core ceilings before the persistence split described in
app/core/README.md (see its 1M TPS realism note).

Usage:  uv run python bench_lob.py [--scale 50000] [--seed 7] [--invariants]
"""

import argparse
import gc
import random
import time
from dataclasses import dataclass

from app.core.lob import (
    LimitOrder,
    LimitOrderBook,
    MarketLimitOrder,
    MarketOrder,
    Rejected,
    Side,
    StopOrder,
    StopTriggered,
    Trade,
    to_ticks,
)


@dataclass(frozen=True, slots=True)
class PhaseResult:
    name: str
    operations: int
    trades: int
    seconds: float
    invariant_seconds: float

    @property
    def ops_per_second(self) -> float:
        return self.operations / self.seconds if self.seconds else 0.0

    @property
    def trades_per_second(self) -> float:
        return self.trades / self.seconds if self.seconds else 0.0


def _timed(
    name: str, book: LimitOrderBook, workload, *, invariant_check: bool
) -> PhaseResult:
    gc.collect()
    started = time.perf_counter()
    operations, trades = workload()
    seconds = time.perf_counter() - started
    invariant_seconds = 0.0
    if invariant_check:
        inv_started = time.perf_counter()
        book.assert_invariants()
        invariant_seconds = time.perf_counter() - inv_started
    return PhaseResult(name, operations, trades, seconds, invariant_seconds)


def _seeded_ladder_price(rng: random.Random, min_price: float) -> float:
    """Random price on the official tick grid at/above `min_price`."""
    min_ticks = to_ticks(min_price)
    return (min_ticks + rng.randint(0, 89)) * 0.01


def build_resting_book(scale: int, rng: random.Random):
    """Eagerly populate a fresh book with non-crossing resting limits."""
    book = LimitOrderBook()
    ids: list[int] = []
    for i in range(scale):
        side = Side.BUY if i % 2 else Side.SELL
        if side is Side.BUY:
            price = _seeded_ladder_price(rng, 99.0)
        else:
            price = _seeded_ladder_price(rng, 101.0)
        events = book.submit(LimitOrder("bench", side, rng.randint(1, 100), price))
        accepted = events[0]
        # resting adds never trade here; both Accepted/Rejected carry .order
        ids.append(accepted.order.order_id)  # type: ignore[union-attr]
    return book, ids


def phase_resting_adds(scale: int, rng: random.Random):
    def workload():
        build_resting_book(scale, rng)
        return scale, 0

    return LimitOrderBook(), workload


def phase_cancels(scale: int, rng: random.Random):
    book, ids = build_resting_book(scale, rng)
    sample = rng.sample(ids, scale // 2)

    def workload():
        cancelled = 0
        for order_id in sample:
            outcome = book.cancel(order_id=order_id, agent_id="bench")
            if not isinstance(outcome, Rejected):
                cancelled += 1
        return cancelled, 0

    return book, workload


def phase_modifies(scale: int, rng: random.Random):
    book, ids = build_resting_book(scale, rng)

    def workload():
        modified = 0
        for order_id in ids:
            book.modify(order_id=order_id, agent_id="bench", quantity=rng.randint(1, 100))
            modified += 1
        return modified, 0

    return book, workload


def phase_taker_sweeps(scale: int, rng: random.Random):
    """Preload a dense ladder on a fresh book; hammer until drained; repeat."""
    levels = min(scale // 100, 400) or 100
    cycles = max(1, min(scale // 10_000, 20))

    def workload():
        ops = trades = 0
        for cycle in range(cycles):
            book = LimitOrderBook()  # carrying leftovers would block valid adds
            for level in range(levels):
                price = round(100.0 + 0.01 * level, 2)
                book.submit(LimitOrder("maker-bench", Side.SELL, rng.randint(20, 200), price))
                ops += 1
            while book.get_best_offer() is not None:
                incoming = rng.randint(1, 150)
                if rng.random() < 0.5:
                    order: MarketOrder | MarketLimitOrder = MarketOrder(
                        "taker-bench", Side.BUY, incoming
                    )
                else:
                    bound = round(99.0 + rng.random() * (levels * 0.01 + 2.0), 2)
                    order = MarketLimitOrder("taker-bench", Side.BUY, incoming, bound)
                events = book.submit(order)
                ops += 1
                trades += sum(isinstance(event, Trade) for event in events)
        return ops, trades

    return LimitOrderBook(), workload


def phase_stop_storm(scale: int, rng: random.Random):
    """Stops arming both sides of a walkable range; the walk fires them."""
    book = LimitOrderBook()
    stops = min(scale // 10, 5000) or 1000
    cycles = max(1, min(scale // 50_000, 10))

    def workload():
        steps = triggers = trades = 0
        for cycle in range(cycles):
            book = LimitOrderBook()  # fresh per cycle: leftover rests would reject valid adds
            # maker liquidity for the converted stops to sweep into
            for i in range(500):
                book.submit(LimitOrder("maker-bench", Side.SELL, rng.randint(10, 80), round(100.0 + 0.01 * i, 2)))
                book.submit(LimitOrder("maker-bench", Side.BUY, rng.randint(10, 80), round(99.99 - 0.01 * i, 2)))
            book.update_market_price(100.0)
            for i in range(stops):
                band = (i // 2) % 200  # keeps stops within ±2.00 of the walk
                if i % 2:
                    book.submit(StopOrder("bench", Side.BUY, rng.randint(1, 40), round(100.0 + 0.01 * (1 + band), 2)))
                else:
                    book.submit(StopOrder("bench", Side.SELL, rng.randint(1, 40), round(100.0 - 0.01 * (1 + band), 2)))
            # rising: fires the buy stops lowest-first until none remain
            while book._book.lowest_stop_buy is not None:
                target = max(book._book.lowest_stop_buy.limit_price * 0.01, 0.01)
                events = book.update_market_price(target)
                steps += 1
                triggers += sum(isinstance(event, StopTriggered) for event in events)
                trades += sum(isinstance(event, Trade) for event in events)
            # falling: fires the sell stops highest-first
            while book._book.highest_stop_sell is not None:
                target = max(book._book.highest_stop_sell.limit_price * 0.01, 0.01)
                events = book.update_market_price(target)
                steps += 1
                triggers += sum(isinstance(event, StopTriggered) for event in events)
                trades += sum(isinstance(event, Trade) for event in events)
        return steps, trades

    return book, workload


def phase_mixed(scale: int, rng: random.Random):
    """Agent-like churn: 55% adds / 20% cancels / 10% modifies / 15% MLs."""
    book = LimitOrderBook()
    book.update_market_price(100.0)
    live_ids: list[int] = []

    def workload():
        operations = trades = 0
        for _ in range(scale):
            roll = rng.random()
            if roll < 0.55 or not live_ids:
                side = Side.BUY if rng.random() < 0.55 else Side.SELL
                if side is Side.BUY:
                    price = _seeded_ladder_price(rng, 99.0)
                else:
                    price = _seeded_ladder_price(rng, 101.0)
                events = book.submit(LimitOrder("bench", side, rng.randint(1, 100), price))
            elif roll < 0.75:
                pick_idx = rng.randrange(len(live_ids))
                picked = live_ids[pick_idx]
                live_ids[pick_idx] = live_ids[-1]
                live_ids.pop()
                events = [book.cancel(order_id=picked, agent_id="bench")]
            elif roll < 0.85:
                events = [
                    book.modify(
                        order_id=rng.choice(live_ids), agent_id="bench", quantity=rng.randint(1, 100)
                    )
                ]
            else:
                events = book.submit(
                    MarketLimitOrder(
                        "bench",
                        Side.BUY if rng.random() < 0.6 else Side.SELL,
                        rng.randint(1, 200),
                        round(98.5 + rng.random() * 3.0, 2),
                    )
                )
            operations += 1
            trades += sum(isinstance(event, Trade) for event in events)
            for event in events:
                new_order = getattr(event, "order", None)
                if new_order is not None and new_order.order_id is not None and (
                    not isinstance(event, Rejected)
                ):
                    live_ids.append(new_order.order_id)
        return operations, trades

    return book, workload


def report(results: list[PhaseResult], scale: int) -> None:
    also = "  (invariants checked per phase)"
    print(f"\nLOB benchmark - scale={scale}{also if results and results[0].invariant_seconds else ''}")
    print(f"{'phase':<16}{'ops':>10}{'trades':>10}{'seconds':>10}{'ops/s':>14}{'trades/s':>12}{'invariant ms':>14}")
    for result in results:
        print(
            f"{result.name:<16}{result.operations:>10}{result.trades:>10}"
            f"{result.seconds:>10.3f}{result.ops_per_second:>14,.0f}"
            f"{result.trades_per_second:>12,.0f}{result.invariant_seconds * 1000:>14.0f}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description="LOB throughput benchmark")
    parser.add_argument("--scale", type=int, default=50_000, help="orders per phase")
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--invariants", action="store_true", help="run assert_invariants after each phase")
    args = parser.parse_args()
    rng = random.Random(args.seed)

    results: list[PhaseResult] = []

    book, workload = phase_resting_adds(args.scale, rng)
    results.append(_timed("resting_adds", book, workload, invariant_check=args.invariants))

    book, workload = phase_cancels(args.scale, rng)
    results.append(_timed("cancels", book, workload, invariant_check=args.invariants))

    book, workload = phase_modifies(args.scale, rng)
    results.append(_timed("modifies", book, workload, invariant_check=args.invariants))

    book, workload = phase_taker_sweeps(args.scale, rng)
    results.append(_timed("taker_sweeps", book, workload, invariant_check=args.invariants))

    book, workload = phase_stop_storm(args.scale, rng)
    results.append(_timed("stop_storm", book, workload, invariant_check=args.invariants))

    book, workload = phase_mixed(args.scale, rng)
    results.append(_timed("mixed_pipeline", book, workload, invariant_check=args.invariants))

    report(results, args.scale)


if __name__ == "__main__":
    main()
