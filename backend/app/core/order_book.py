"""Limit Order Book (LOB) matching engine.

Acts as the virtual stock exchange. Single source of truth for simulated
market state: agents submit orders, but only the engine mutates the book
(single-writer principle, no race conditions). Matches on price-time priority.
"""


class LimitOrderBook:
    """Stores outstanding buy/sell orders and matches them."""

    # TODO: implement order submission, cancellation and price-time matching
