"""API layer entrypoint.

Exposes simulation state (from Redis) and control endpoints to the frontend
dashboard. Run with: uv run uvicorn app.main:app --reload
"""

from fastapi import FastAPI

app = FastAPI(title="Market Simulation Backend")


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}
