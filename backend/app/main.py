"""API layer entrypoint.

Exposes simulation state (from Redis), auth, and control endpoints to the
frontend dashboard. Run with: uv run uvicorn app.main:app --reload
"""

from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from app.auth import router as auth_router
from app.auth import service as auth_service
from app.db import init_db

# The dashboard dev server origin; cookies are forwarded through the
# /api/auth proxy rewrite.
CORS_ORIGINS = ["http://localhost:3000"]


@asynccontextmanager
async def lifespan(app: FastAPI):
    await init_db()
    yield


app = FastAPI(title="Market Simulation Backend", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=CORS_ORIGINS,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(auth_router)


@app.exception_handler(auth_service.AuthError)
async def auth_error_handler(request: Request, exc: auth_service.AuthError) -> JSONResponse:
    return JSONResponse(
        status_code=exc.status,
        content={"message": exc.message, "code": exc.code},
    )


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}
