"""Redis fast read layer.

Stores current order book, latest trades, market snapshots and agent state
for fast/frequent access by agents and the API layer. Also carries the
Redis Streams order queue from agents to the matching engine.
"""

import redis.asyncio as redis

from app.config import settings


def get_redis() -> redis.Redis:
    return redis.from_url(settings.redis_url, decode_responses=True)
