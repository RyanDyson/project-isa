"""Long-term persistence through the SQLAlchemy ORM (see app/db/base.py).

Retains order submissions, trade executions, cancellations, fee structures,
planner adjustments, agent portfolio balances and simulation evaluation
metrics - enabling replay of runs and comparison between policy frameworks.
"""

from app.db import get_session

__all__ = ["get_session"]
