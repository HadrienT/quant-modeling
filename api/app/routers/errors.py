"""HTTP errors several routers raise the same way."""

from fastapi import HTTPException


def store_unavailable() -> HTTPException:
    """503 for a db.StoreUnavailable: the market data store cannot be reached.
    The cause stays in the logs (raise ... from exc), not in the response."""
    return HTTPException(status_code=503, detail="Market data store unavailable")
