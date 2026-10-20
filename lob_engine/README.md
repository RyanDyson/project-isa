# Simulation Core

## Limit Order Book (LOB)

### Overview

Acts as the virtual stock exchange and serves as the single source of truth for the simulated market state.
It is built in C++ and should be able to handle +1,000,000 TPS (trades per second).

### Background information

The matching is a core part of any electronic exchange. Its where buyers and traders meet and executes trades by matching order prices. The matching engine maintains the limit order book and contains all oders that have yet to be matched. Since the matching engine is the sole executor of trades, we need to make a fast matching engine to avoid latency and delay. 

### Supported Order Types

The order types that will be covered by the LOB is as follow,

| Type | Behavior on Submit | Modify / Cancel |
| --- | --- | --- |
| **MarketOrder** | Executes immediately at the best available market price. Unfilled remainder is canceled. | **No** |
| **LimitOrder** | Rests at its limit price without instant trades. The engine rejects crossing submissions (use `MarketLimitOrder` for crossing). | **Yes** (while resting) |
| **MarketLimitOrder** | Limit order that crosses the book: executes in a taker phase first, then any remainder rests at the limit price. | **Yes** (resting remainder) |
| **StopOrder** | Remains dormant until the market hits the trigger price, then converts into a **MarketOrder**. | **Yes** (pre-trigger) |
| **StopLimitOrder** | Remains dormant until the market hits the trigger price, then converts into a **LimitOrder** (allowed to cross). | **Yes** (pre-trigger) |

### Idea 
![alt text](./assets/image.png)

Following implementations of actual LOB and matching engines, we will use a FIFO and Price-Time priority to determine the execution order of entries in the LOB. 

![alt text](./assets/image-1.png)

* **Data Structure:** Requests are organized in balanced binary trees (one for **Buy** orders and one for **Sell** orders).
* **AVL Trees:** Each binary tree is implemented as an **AVL tree**, where each node represents a specific **price level** to keep the tree balanced and ensure $O(\log M)$ search/insertion times.
* **Node:** Each node (price level) is a doubly linked list inserted based on order submission time (FIFO).

### Architecture

```text
src/core/book.hpp           public API: OrderBook, SubmitRequest/ModifyRequest, Event/EventSink
src/core/book.cpp           thin facade over the internal State class
src/core/nodes.hpp          internal node types: Order (intrusive FIFO), PriceLevel (AVL node)
src/core/level_tree.hpp     AVL tree of price levels (structure + inline hot paths)
src/core/level_tree.cpp     AVL implementation (insert/remove/rebalance)
src/core/state.hpp          internal engine state: data + inline link/unlink + declarations
src/core/state_pools.cpp    constructor, object pools, price-level book-keeping
src/core/state_matching.cpp taker sweeps, taker lifecycle, dormant-stop trigger drain
src/core/state_commands.cpp validation + submit / cancel / modify
src/core/state_queries.cpp  queries + full invariant audit
tests/                      self-contained test harness + scenarios (no deps)
bench/                      throughput benchmark (no deps)
```

Builds link with `-flto` so cross-module calls stay inlined (throughput is
at parity with the pre-split single-file engine).

- **Data structures:** one AVL tree per side keyed by price (bids, asks) plus
  two dormant-stop trees keyed by trigger price. Each tree node owns an
  intrusive doubly-linked FIFO list of pool-allocated orders (time priority).
- **O(1) paths:** cache of the best level per tree (`best`), a
  `price -> level` hash index per side for `volume_at`, and an
  `order id -> order` map for `cancel`/`modify` (intrusive unlink only).
- **Matching:** taker phase sweeps the opposite side level by level, FIFO
  within each level; the trade price is the maker's resting price. Pure
  `LimitOrder` submissions that would cross are rejected; `MarketableLimit`
  and remaining stop conversions rest the unfilled remainder.
- **Stop triggers:** every trade updates the last traded price; a drain loop
  then activates dormant stops in trigger order (buys ascending, sells
  descending, FIFO inside a trigger) iteratively, so arbitrarily deep cascades
  cannot blow the stack.
- **Events:** every accepted/rejected/modified/cancelled order, every trade,
  and every fill is published to an `EventSink` synchronously -- the Redis
  microservice layer journals these downstream.


### Time Complexity

- Add Order – O(log M) for the first order at a limit, O(1) for all others, where M is the number of price Limits (generally << N the number of orders).
- Cancel Order – O(1)
- Modify Order – O(1)
- Execute – O(1)
- GetVolumeAtLimit – O(1)
- GetBestBid/Offer – O(1)

### Constraints
- Order shares > 0
- PPS > 0
- Order Ids are unique

### References
https://github.com/brprojects/Limit-Order-Book/tree/main

---

## Verification

The core ships with its own harness (no external dependencies):

```bash
# build + run the test suite (26 tests, includes a randomized stress test
# that re-verifies all invariants after every single operation)
docker run --rm -v "$(pwd)":/work -w /work gcc:13 bash scripts/build_and_test.sh

# build + run tests, then the throughput benchmark
docker run --rm --cpus 4 -v "$(pwd)":/work -w /work gcc:13 bash scripts/build_and_test.sh --bench
```

CMake targets: `lob_core` (library), `lob-core-tests`, `lob-core-bench`.

`OrderBook::audit()` verifies all internal invariants on demand (AVL heights,
balance factors, parent pointers, strict key ordering, FIFO links, per-level
total-vs-sum, id-map bijection, price index consistency); the stress test and
the benchmark both call it.

### Throughput

Single-threaded in-memory core (no serialization, no Redis; Docker Desktop
on Windows, `gcc:13 -O3`, two-sided book, 201 price levels):

| scenario        | ops/s       | trades/s |
| --------------- | ----------- | -------- |
| build-depth     | ~15 million | -        |
| steady-matching | ~28 million | ~7.3 M   |
| cancel-churn    | ~7.3 million| -        |
| mixed-workload  | ~18 million | ~3.0 M   |

Per-operation latency (sampled 1-in-16; partial timing includes ~45 ns of
two `steady_clock` calls):

| workload | p50     | p90     | p99     | p99.9   |
| -------- | ------- | ------- | ------- | ------- |
| matching | 62 ns   | 85 ns   | ~130 ns | ~260 ns |
| cancel   | 42 ns   | 45 ns   | ~55 ns  | ~65 ns  |

The cancel path is an O(1) intrusive unlink; its true cost sits at the
clock-measurement floor.

**Scaling with book size** (`LOB_LEVELS=N` widens the depth scenarios):

| price levels | build-depth ops/s | steady-matching ops/s |
| ------------ | ----------------- | --------------------- |
| 201          | ~15.1 M           | ~28 M                 |
| 2000         | ~15.1 M           | ~29 M                 |
| 10000        | ~11.5 M           | ~18 M                 |

Throughput stays essentially flat up to a few thousand levels (the AVL
insert fix-up stops early at the first ancestor whose subtree height is
unchanged) and degrades gracefully at 10000 levels where traversal pointer
chasing starts missing cache.

Every scenario re-audits the book afterwards; a violation aborts with a
description.

---

## Metrics Aggregator

