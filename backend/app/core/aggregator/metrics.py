"""Market Metrics Aggregator.

Turns order book activity into market-condition signals: volatility,
bid-ask spread, liquidity depth, trading volume, cancellation rate and
order-to-trade ratio. Consumed by agents, the RL planner and the dashboard.
Backed by NumPy/Pandas.
"""


class MetricsAggregator:
    """Computes rolling market metrics from book and trade events."""

    # TODO: implement metric calculations
