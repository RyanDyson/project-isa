"""Authentication package.

Implements a Better Auth-compatible email/password auth API in FastAPI so the
Next.js dashboard (which keeps using the `better-auth` React client) can talk
to the Python backend directly through the /api/auth proxy rewrite.
"""

from app.auth.routes import router

__all__ = ["router"]
