# Redis Schema (hot layer)

All keys are scoped by run: `lob:{run}` prefix — a run's state is one SCAN away.
Written by two writers, both derived from the C++ engine's single-threaded loop
(rebuildable at any time; event_log in PostgreSQL stays the source of truth):

- **lob-engine (C++)**: journals engine events, mirrors the reads-matters keys
  below after every command (ladder + order hashes + agent index + tape).
- **backend (python)**: run lifecycle marker + meta, and the Postgres flusher
  (consumer group `pg-flusher` on the event streams; XACK only after commit).

Convention: prices are integer ticks everywhere (`PRICE_TICK = 0.01`).

## Keys

| Key | Type | Purpose | Writer |
| --- | --- | --- | --- |
| `lob:{run}:meta` | HASH | `started_at`, `mode`, `seed`, `params` | backend |
| `lob:{run}:symbols` | SET | Tickers this run has touched (discovery for the flusher, `/symbols` route) | engine |
| `lob:active-run` | STRING (TTL 60 s) | The active run id; the engine polls this to attach/detach | backend |
| `lob:{run}:commands` | Stream | Engine commands: one field `payload` (JSON command) | producers (REST bridge) |
| `lob:{run}:reply:{corr_id}` | STRING (TTL 8 s) | Command reply: `{corr_id, status, events}` | engine |
| `lob:{run}:{symbol}:events` | Stream | Engine event journal (`accepted`/`modified`/`cancelled`/`stop-triggered`/`trade`); fields `seq`, `type`, `ts`, `payload` | engine |
| `lob:{run}:{symbol}:bids` / `:asks` | ZSET | Ordered ladders. member = price ticks (string), score = same int. Members removed when the level empties. Range queries serve depth views. | engine |
| `lob:{run}:{symbol}:order:{id}` | HASH | Live order state: `agent`, `side`, `type`, `quantity`, `remaining`, `limit_ticks`, `stop_ticks`, `status`, `seq`. DEL on cancel/fill. | engine |
| `lob:{run}:agent:{agent_id}:orders` | ZSET | Open-orders index per agent. member = `{symbol}:{order_id}`, score = seq. ZREM on cancel/fill. | engine |
| `lob:{run}:{symbol}:level:{side}:{ticks}` | HASH | Per-level aggregates: `size` (order count), `volume`. Covers GetVolumeAtLimit reads without touching ZSET members. | engine |
| `lob:{run}:{symbol}:trades` | Stream (MAXLEN ~10k) | Trade tape: fields `seq`, `buy_order_id`, `sell_order_id`, `buy_agent`, `sell_agent`, `price_ticks`, `quantity`, `aggressor_side`, `ts`. | engine |
| `lob:{run}:{symbol}:last` | STRING | Last traded price ticks (also derivable: tail of trades stream). | engine |

## Ladder ZSET + HASH detail

- Engine rebuilds the touched symbol's ladder (DEL zset + ZADD each resting
  price + HSET level hash) after every command — cheap at dev scale.
- Reads: ladder membership from the ZSET (ZRANGE/ZREVRANGE), aggregates from
  the level HASH (`size` = order count, `volume` = quantity).

## Writer protocol

- The python flusher reads each symbol's events stream in batches of <= 2000,
  tracks the committed cursor implicitly via the consumer group, and acks
  entries only after their PG commit lands.
- Event journal `payload` shapes mirror the python engine's previous journal
  (see `app/storage/pipeline.py` `_build_payload` consumers): trade entries
  carry `affected` order states (ticks + remaining + status) so the flusher
  upserts the Order/Trade projection tables.
- Run teardown: `FLUSH` of `lob:{run}:*` keys after the final PG flush.
