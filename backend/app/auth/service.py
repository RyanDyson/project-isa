"""Password hashing and session management for the auth API.

Password hashes are stored in the `account` table (provider_id="credential")
in `salt:hash` format, using scrypt with the same parameters Better Auth uses
(N=16384, r=8, p=1, 64-byte key, hex encoded), keeping the database compatible
with existing better-auth tooling.
"""

import hashlib
import hmac
import secrets
from datetime import UTC, datetime, timedelta

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.models import Account, Session, User

SCRYPT_N = 16384
SCRYPT_R = 8
SCRYPT_P = 1
SCRYPT_KEYLEN = 64
SESSION_LIFETIME = timedelta(days=7)

# Cookie name must match what the better-auth client expects.
SESSION_COOKIE = "better-auth.session_token"


class AuthError(Exception):
    """Auth failure with an HTTP status and better-auth style code."""

    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


def hash_password(password: str) -> str:
    salt = secrets.token_hex(16)
    return f"{salt}:{_scrypt(password, salt)}"


def verify_password(password: str, stored: str) -> bool:
    try:
        salt, expected = stored.split(":", 1)
    except ValueError:
        return False
    return hmac.compare_digest(_scrypt(password, salt), expected)


def _scrypt(password: str, salt: str) -> str:
    digest = hashlib.scrypt(
        password.encode("utf-8"),
        salt=salt.encode("utf-8"),
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        dklen=SCRYPT_KEYLEN,
    )
    return digest.hex()


def session_expiry(now: datetime | None = None) -> datetime:
    return (now or datetime.now(UTC)) + SESSION_LIFETIME


async def get_user_by_email(db: AsyncSession, email: str) -> User | None:
    return (await db.execute(select(User).where(User.email == email))).scalar_one_or_none()


async def get_credential_account(db: AsyncSession, user_id: str) -> Account | None:
    return (
        await db.execute(
            select(Account).where(
                Account.user_id == user_id,
                Account.provider_id == "credential",
            )
        )
    ).scalar_one_or_none()


async def get_session_by_token(db: AsyncSession, token: str) -> tuple[Session, User] | None:
    row = (
        await db.execute(
            select(Session, User)
            .join(User, Session.user_id == User.id)
            .where(Session.token == token, Session.expires_at > datetime.now(UTC))
        )
    ).first()
    return row


def user_dict(user: User) -> dict:
    """Serialize a user in the better-auth client wire format (camelCase)."""
    return {
        "id": user.id,
        "name": user.name,
        "email": user.email,
        "emailVerified": user.email_verified,
        "image": user.image,
        "createdAt": user.created_at.isoformat(),
        "updatedAt": user.updated_at.isoformat(),
    }


def session_dict(session: Session) -> dict:
    return {
        "id": session.id,
        "token": session.token,
        "userId": session.user_id,
        "expiresAt": session.expires_at.isoformat(),
        "createdAt": session.created_at.isoformat(),
        "updatedAt": session.updated_at.isoformat(),
        "ipAddress": session.ip_address,
        "userAgent": session.user_agent,
    }
