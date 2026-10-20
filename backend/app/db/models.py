"""ALL database models in one place (auth + market persistence).

Auth table/column names match the Better Auth default schema exactly.
Market tables are keyed (run_id, ...) so an experiment run is one atom;
orders/trades are projections of the event journal.
"""

from datetime import datetime

from sqlalchemy import (
    BigInteger,
    Boolean,
    DateTime,
    ForeignKey,
    Index,
    Integer,
    String,
    Text,
)
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base


# ---------------------------------------------------------------------------
# Auth (Better Auth-compatible)
# ---------------------------------------------------------------------------


class User(Base):
    __tablename__ = "user"

    id: Mapped[str] = mapped_column(Text, primary_key=True)
    name: Mapped[str] = mapped_column(Text, nullable=False)
    email: Mapped[str] = mapped_column(Text, unique=True, nullable=False)
    email_verified: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    image: Mapped[str | None] = mapped_column(Text)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)


class Session(Base):
    __tablename__ = "session"

    id: Mapped[str] = mapped_column(Text, primary_key=True)
    expires_at: Mapped[datetime] = mapped_column("expires_at", DateTime(timezone=True), nullable=False)
    token: Mapped[str] = mapped_column(Text, unique=True, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    ip_address: Mapped[str | None] = mapped_column("ip_address", Text)
    user_agent: Mapped[str | None] = mapped_column("user_agent", Text)
    user_id: Mapped[str] = mapped_column(
        "user_id",
        String(255),
        ForeignKey("user.id", ondelete="cascade"),
        nullable=False,
    )


class Account(Base):
    __tablename__ = "account"

    id: Mapped[str] = mapped_column(Text, primary_key=True)
    account_id: Mapped[str] = mapped_column("account_id", Text, nullable=False)
    provider_id: Mapped[str] = mapped_column("provider_id", Text, nullable=False)
    user_id: Mapped[str] = mapped_column(
        "user_id",
        String(255),
        ForeignKey("user.id", ondelete="cascade"),
        nullable=False,
    )
    access_token: Mapped[str | None] = mapped_column("access_token", Text)
    refresh_token: Mapped[str | None] = mapped_column("refresh_token", Text)
    id_token: Mapped[str | None] = mapped_column("id_token", Text)
    access_token_expires_at: Mapped[datetime | None] = mapped_column(
        "access_token_expires_at", DateTime(timezone=True)
    )
    refresh_token_expires_at: Mapped[datetime | None] = mapped_column(
        "refresh_token_expires_at", DateTime(timezone=True)
    )
    scope: Mapped[str | None] = mapped_column(Text)
    password: Mapped[str | None] = mapped_column(Text)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)


class Verification(Base):
    __tablename__ = "verification"

    id: Mapped[str] = mapped_column(Text, primary_key=True)
    identifier: Mapped[str] = mapped_column(Text, nullable=False)
    value: Mapped[str] = mapped_column(Text, nullable=False)
    expires_at: Mapped[datetime] = mapped_column("expires_at", DateTime(timezone=True), nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)


# ---------------------------------------------------------------------------
# Market persistence
# ---------------------------------------------------------------------------


class Run(Base):
    __tablename__ = "runs"

    run_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    started_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    ended_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    mode: Mapped[str] = mapped_column(String(32), default="manual", nullable=False)
    seed: Mapped[int | None] = mapped_column(Integer)
    params: Mapped[dict | None] = mapped_column(JSONB)


class EventLog(Base):
    """Canonical journal: one row per engine event, ordered by per-run seq."""

    __tablename__ = "event_log"

    run_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    seq: Mapped[int] = mapped_column(BigInteger, primary_key=True)
    symbol: Mapped[str] = mapped_column(String(16), nullable=False)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    event_type: Mapped[str] = mapped_column(String(32), nullable=False)
    payload: Mapped[dict] = mapped_column(JSONB, nullable=False)

    __table_args__ = (Index("ix_event_log_run_symbol_seq", "run_id", "symbol", "seq"),)


class Order(Base):
    """Projection of submissions/updates: one lifecycle row per order."""

    __tablename__ = "orders"

    run_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    order_id: Mapped[int] = mapped_column(BigInteger, primary_key=True)
    symbol: Mapped[str] = mapped_column(String(16), nullable=False)
    agent_id: Mapped[str] = mapped_column(String(64), nullable=False)
    side: Mapped[str] = mapped_column(String(4), nullable=False)
    order_type: Mapped[str] = mapped_column(String(16), nullable=False)
    quantity: Mapped[int] = mapped_column(BigInteger, nullable=False)

    limit_ticks: Mapped[int | None] = mapped_column(Integer)
    stop_ticks: Mapped[int | None] = mapped_column(Integer)

    status: Mapped[str] = mapped_column(String(16), nullable=False)
    created_seq: Mapped[int] = mapped_column(BigInteger, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    updated_seq: Mapped[int | None] = mapped_column(BigInteger)
    updated_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    parent_order_id: Mapped[int | None] = mapped_column(BigInteger)

    __table_args__ = (Index("ix_orders_run_agent_status", "run_id", "agent_id", "status"),)


class Trade(Base):
    """One executed fill, append-only."""

    __tablename__ = "trades"

    run_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    seq: Mapped[int] = mapped_column(BigInteger, primary_key=True)
    symbol: Mapped[str] = mapped_column(String(16), nullable=False)
    buy_order_id: Mapped[int] = mapped_column(BigInteger, nullable=False)
    sell_order_id: Mapped[int] = mapped_column(BigInteger, nullable=False)
    buy_agent: Mapped[str] = mapped_column(String(64), nullable=False)
    sell_agent: Mapped[str] = mapped_column(String(64), nullable=False)
    price_ticks: Mapped[int] = mapped_column(Integer, nullable=False)
    quantity: Mapped[int] = mapped_column(BigInteger, nullable=False)
    aggressor_side: Mapped[str] = mapped_column(String(4), nullable=False)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)

    __table_args__ = (Index("ix_trades_run_symbol_ts", "run_id", "symbol", "ts"),)


class BookSnapshot(Base):
    """Periodic full-ladder checkpoint per symbol for fast recovery."""

    __tablename__ = "book_snapshots"

    run_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    seq: Mapped[int] = mapped_column(BigInteger, primary_key=True)
    symbol: Mapped[str] = mapped_column(String(16), nullable=False)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    payload: Mapped[dict] = mapped_column(JSONB, nullable=False)
