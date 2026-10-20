"""Better Auth-compatible auth endpoints.

The Next.js dashboard proxies /api/auth/* here and keeps using the standard
better-auth/auth-client, so this module must produce responses and Set-Cookie
headers in the shape better-auth expects:

- POST /api/auth/sign-up/email  -> 200 user JSON; sets session cookie
- POST /api/auth/sign-in/email  -> 200 user JSON; sets session cookie
- GET  /api/auth/get-session    -> 200 {session, user} JSON or null
- POST /api/auth/sign-out       -> revokes the session; clears the cookie
"""

import secrets
from datetime import UTC, datetime

from fastapi import APIRouter, Depends, Request, Response
from pydantic import BaseModel
from sqlalchemy import delete as sqlalchemy_delete
from sqlalchemy.ext.asyncio import AsyncSession

from app.auth import service
from app.db.models import Account, Session, User
from app.db import get_session

router = APIRouter(prefix="/api/auth", tags=["auth"])

# better-auth's default cookie attributes
COOKIE_PATH = "/"
COOKIE_SAMESITE = "lax"


def _now() -> datetime:
    return datetime.now(UTC)


def _new_id() -> str:
    return secrets.token_hex(16)


def _set_session_cookie(response: Response, token: str, expires: datetime) -> None:
    response.set_cookie(
        service.SESSION_COOKIE,
        token,
        expires=expires.strftime("%a, %d %b %Y %H:%M:%S GMT"),
        path=COOKIE_PATH,
        httponly=True,
        samesite=COOKIE_SAMESITE,
        secure=False,  # local development runs over http
    )


def _clear_session_cookie(response: Response) -> None:
    response.delete_cookie(service.SESSION_COOKIE, path=COOKIE_PATH)


class SignUpBody(BaseModel):
    name: str
    email: str
    password: str


class SignInBody(BaseModel):
    email: str
    password: str


@router.post("/sign-up/email")
async def sign_up_email(
    body: SignUpBody,
    request: Request,
    response: Response,
    db: AsyncSession = Depends(get_session),
) -> dict:
    if len(body.password) < 8:
        raise service.AuthError(422, "INVALID_PASSWORD", "Password must be at least 8 characters")
    if await service.get_user_by_email(db, body.email):
        raise service.AuthError(422, "USER_ALREADY_EXISTS", "User already exists")

    now = _now()
    user = User(
        id=_new_id(),
        name=body.name,
        email=body.email,
        email_verified=False,
        image=None,
        created_at=now,
        updated_at=now,
    )
    account = Account(
        id=_new_id(),
        account_id=user.id,
        provider_id="credential",
        user_id=user.id,
        password=service.hash_password(body.password),
        created_at=now,
        updated_at=now,
    )
    expiry = service.session_expiry(now)
    session = Session(
        id=_new_id(),
        expires_at=expiry,
        token=secrets.token_hex(32),
        created_at=now,
        updated_at=now,
        ip_address=_client_ip(request),
        user_agent=request.headers.get("user-agent"),
        user_id=user.id,
    )

    db.add(user)
    await db.flush()  # ensure user.id is persisted before account/session FKs
    db.add_all([account, session])
    await db.commit()

    _set_session_cookie(response, session.token, expiry)
    return service.user_dict(user)


@router.post("/sign-in/email")
async def sign_in_email(
    body: SignInBody,
    request: Request,
    response: Response,
    db: AsyncSession = Depends(get_session),
) -> dict:
    user = await service.get_user_by_email(db, body.email)
    account = await service.get_credential_account(db, user.id) if user else None
    if (
        user is None
        or account is None
        or account.password is None
        or not service.verify_password(body.password, account.password)
    ):
        raise service.AuthError(401, "INVALID_EMAIL_OR_PASSWORD", "Invalid email or password")

    now = _now()
    expiry = service.session_expiry(now)
    session = Session(
        id=_new_id(),
        expires_at=expiry,
        token=secrets.token_hex(32),
        created_at=now,
        updated_at=now,
        ip_address=_client_ip(request),
        user_agent=request.headers.get("user-agent"),
        user_id=user.id,
    )
    db.add(session)
    await db.commit()

    _set_session_cookie(response, session.token, expiry)
    return service.user_dict(user)

@router.get("/get-session")
async def current_session(request: Request, db: AsyncSession = Depends(get_session)) -> dict | None:
    token = request.cookies.get(service.SESSION_COOKIE)
    if not token:
        return None

    found = await service.get_session_by_token(db, token)
    if found is None:
        return None

    session, user = found
    return {"session": service.session_dict(session), "user": service.user_dict(user)}


@router.post("/sign-out")
async def sign_out(request: Request, response: Response, db: AsyncSession = Depends(get_session)) -> None:
    token = request.cookies.get(service.SESSION_COOKIE)
    if token:
        await db.execute(sqlalchemy_delete(Session).where(Session.token == token))
        await db.commit()
    _clear_session_cookie(response)


def _client_ip(request: Request) -> str | None:
    forwarded = request.headers.get("x-forwarded-for")
    if forwarded:
        return forwarded.split(",")[0].strip()
    return request.client.host if request.client else None
