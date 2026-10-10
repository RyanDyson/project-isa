"""Database layer: async SQLAlchemy engine, session factory, and base class.

The DSN is stored in settings.database_url (see .env / .env.example) and uses
the asyncpg driver: postgresql+asyncpg://user:password@host:port/dbname
"""

from collections.abc import AsyncIterator

from sqlalchemy.ext.asyncio import (
    AsyncEngine,
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)
from sqlalchemy.orm import DeclarativeBase

from app.config import settings


class Base(DeclarativeBase):
    """Declarative base for all ORM models."""


engine: AsyncEngine = create_async_engine(settings.database_url, echo=False)

SessionMaker = async_sessionmaker(engine, class_=AsyncSession, expire_on_commit=False)


async def get_session() -> AsyncIterator[AsyncSession]:
    """FastAPI dependency yielding a transactional session."""
    async with SessionMaker() as session:
        yield session


async def init_db() -> None:
    """Create all tables (dev convenience; use Alembic migrations later)."""
    # Import all model modules so their tables register on Base.metadata.
    import app.auth.models  # noqa: F401

    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
