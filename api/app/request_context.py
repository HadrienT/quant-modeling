"""Request-scoped state, shared through ContextVars so code nested deep in a
call (e.g. a fallback decided inside vol_surface.py) can reach it without a
`Request` object being threaded through every function. `AuditMiddleware`
(audit/middleware.py) sets request_id/ip_hash/username once per request;
`emit()` reads them as the defaults for every event's envelope.
"""

from contextvars import ContextVar

cache_hit_context: ContextVar[bool] = ContextVar("cache_hit", default=False)
request_id_context: ContextVar[str | None] = ContextVar("request_id", default=None)
ip_hash_context: ContextVar[str | None] = ContextVar("ip_hash", default=None)
username_context: ContextVar[str | None] = ContextVar("username", default=None)


def set_cache_hit():
    """Mark current request as cache hit."""
    cache_hit_context.set(True)


def is_cache_hit() -> bool:
    """Check if current request hit cache."""
    return cache_hit_context.get()


def reset_cache_hit():
    """Reset cache hit status (called at start of each request)."""
    cache_hit_context.set(False)


def set_request_context(
    *, request_id: str | None, ip_hash: str | None, username: str | None
) -> None:
    request_id_context.set(request_id)
    ip_hash_context.set(ip_hash)
    username_context.set(username)


def reset_request_context() -> None:
    request_id_context.set(None)
    ip_hash_context.set(None)
    username_context.set(None)


def current_request_id() -> str | None:
    return request_id_context.get()


def current_ip_hash() -> str | None:
    return ip_hash_context.get()


def current_username() -> str | None:
    return username_context.get()


def current_trace_id() -> str | None:
    # OpenTelemetry instrumentation lands in lot 18d; no span context to read yet.
    return None
