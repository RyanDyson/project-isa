# Simulation Core

## Limit Order Book (LOB)

### Overview

Acts as the virtual stock exchange and serves as the single source of truth for the simulated market state.
It is built in C++ (microservice: `../lob_engine/`) and handles +1,000,000 TPS. The FastAPI backend
no longer matches orders itself; its `/api/market/*` routes translate REST into engine commands over
Redis Streams and wait for correlated replies.

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

### Architecture

#### Component Map

- **C++ matching core** — `lob_engine/src/core/`: AVL trees of price levels per side,
  intrusive FIFO lists for time priority, pool allocation, price index for O(1) volume
  reads, dormant-stop books with cascading triggers. Details + testing/benchmarking
  instructions in `lob_engine/README.md`.
- **C++ service wire layer** — `lob_engine/src/main.cpp`: consumes
  `lob:{run}:commands`, journals engine events to `lob:{run}:{symbol}:events`, replies
  to `lob:{run}:reply:{corr_id}`, and mirrors the hot Redis layer after every command.
- **Backend storage** — `app/storage/pipeline.py`: run lifecycle marker (TTL'd) and the
  Postgres flusher (journal + orders + trades, grouped `pg-flusher` on the streams).
- **Backend REST bridge** — `app/storage/engine_client.py` + `app/api/market.py`: build
  commands, poll correlated replies, serve depth/tape reads from the hot layer.

#### Redis protocol

Key contract: `app/storage/redis_schema.md`.

### Time Complexity (matching core)

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

## Metrics Aggregator
