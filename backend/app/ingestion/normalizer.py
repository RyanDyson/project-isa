"""Data Normalizer and Book Seeder.

Converts incoming Alpaca data (REST historical / WebSocket live stream of
trades, quotes, market updates) into a consistent internal format by
standardizing timestamps, symbols, prices, quantities and event types.
Normalized data initializes or updates the simulated market.
"""

# TODO: normalization schema
# TODO: book seeding from normalized events
