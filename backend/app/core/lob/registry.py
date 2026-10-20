"""Engine-per-symbol registry: one single-writer book per ticker."""

import re

from .engine import LimitOrderBook

_TICKER_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,16}$")


class SymbolRegistry:
    """Lazy map of ticker -> its own LimitOrderBook.

    Routing by symbol keeps the matching core untouched: books remain
    independent (no cross-ticker matching), each with its own ids, senders,
    and stop registry. Cross-ticker analytics belong to the metrics layer.
    """

    def __init__(self) -> None:
        self._books: dict[str, LimitOrderBook] = {}

    @staticmethod
    def normalize(symbol: str) -> str:
        if not _TICKER_PATTERN.fullmatch(symbol):
            raise ValueError(f"invalid ticker '{symbol}': 1-16 chars of A-Z 0-9 . _ -")
        return symbol.upper()

    def book(self, symbol: str) -> LimitOrderBook:
        return self._books.setdefault(self.normalize(symbol), LimitOrderBook())

    def symbols(self) -> list[str]:
        return sorted(self._books)

    def drop(self, symbol: str) -> bool:
        return self._books.pop(self.normalize(symbol), None) is not None

    def __len__(self) -> int:
        return len(self._books)

    def __contains__(self, symbol: str) -> bool:
        return self.normalize(symbol) in self._books
