import os

from fastapi import Header, HTTPException


def require_api_key(x_api_key: str | None = Header(default=None)) -> None:
    """Legacy server-to-server key check. Kept for a reverse proxy that still
    wants to gate a route; no browser route uses it (ADR-008)."""
    expected = os.getenv("API_KEY")
    if expected and x_api_key != expected:
        raise HTTPException(status_code=401, detail="Unauthorized")
