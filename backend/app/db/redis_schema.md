# Redis Schema (hot layer)

All keys are scoped by run: `lob:{run}` prefix — a run's state is one SCAN away.
Written by a pipelined background task drained from the engine's event ring
buffer (never in the matching loop). Rebuildable at any time from PostgreSQL.

Convention: prices are integer ticks everywhere (`PRICE_TICK = 0.01`).

## Keys

| Key | Type | Purpose |
| --- | --- | --- |
| `lob:{run}:meta` | HASH | `started_at`, `mode`, `seed`, `params` |
| `lob:{run}:{symbol}:bids` / `:asks` | ZSET | Ordered ladders. member = price ticks (string), score = same int. Members removed when the level empties. Range queries serve depth views. |
| `lob:{run}:{symbol}:orders:{id}` | HASH | Live order state: `agent`, `side`, `type`, `quantity`, `remaining`, `limit_ticks`, `stop_ticks`, `status`, `seq`. DEL on cancel/fill. |
| `lob:{run}:agent:{agent_id}:orders` | ZSET | Open-orders index per agent. member = `{symbol}:{order_id}`, score = seq. ZREM on cancel/fill. |
| `lob:{run}:{symbol}:level:{side}:{ticks}` | HASH | Per-level aggregates: `size` (order count), `volume`. Covers GetVolumeAtLimit reads without touching ZSET members. DEL'd when emptied. |
| `lob:{run}:{symbol}:trades` | Stream (MAXLEN ~10k) | Trade tape: fields `seq`, `buy_order_id`, `sell_order_id`, `buy_agent`, `sell_agent`, `price_ticks`, `quantity`, `aggressor_side`, `ts`. |
| `lob:{run}:{symbol}:events` | Stream | Engine event journal buffer (accepted/modified/cancelled/stop-triggered/trade). Consumed by the Postgres flusher (XREAD group `flusher`) and metrics/planner consumers. `XACK` only after PG commit. |
| `lob:{run}:{symbol}:last` | STRING | Last traded price ticks (also derivable: tail of trades stream). |

## Level ZSET + HASH detail

- Resting insert: `ZADD {symbol}:bids <price> <price>` + `HSET ...:level:buy:<price> size 1 volume Q`
- Fill/decrement: `HINCRBY level size -1 volume -Q`, `HSET order:* remaining R` (+ ZADD/ZREM of order ladder? no — only level membership)
- Level empties: `ZREM {symbol}:bids <price>` + `DEL level hash`

## Writer protocol

- Pipelined batches: <= 512 events or 5 ms, whichever first.
- Event stream entries carry the engine `seq` — the flusher tracks
  `lob:{run}:flush_cursor` (last committed seq) and acks entries only after
  their PG commit lands.
- Run teardown: `FLUSH` of `lob:{run}:*` keys after the final PG flush.
