"""PostgreSQL long-term persistence.

Retains order submissions, trade executions, cancellations, fee structures,
planner adjustments, agent portfolio balances and simulation evaluation
metrics - enabling replay of runs and comparison between policy frameworks.
"""

import asyncpg

from app.config import settings


async def get_connection() -> asyncpg.Connection:
    return await asyncpg.connect(dsn=settings.database_url)
