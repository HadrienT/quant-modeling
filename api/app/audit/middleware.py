"""HTTP access auditing (WP §5.1). Replaces `CacheLoggingMiddleware`: a
free-text log line per request becomes a typed `http.access` event, with a
normalized route (the path template, e.g. `/api/pricing/{product}` — never the
raw path, which would make `route` a high-cardinality label if it ever fed a
metric, WP principle 8) and a `request_id` every response carries back in
`X-Request-ID`.

This is also the one place that decodes the bearer token once per request: it
is what lets `record_fallback()` deep in `vol_surface.py`, or `auth.py`'s own
handlers, pick up `username`/`ip_hash` from context instead of needing a
`Request` threaded through every call.
"""

from __future__ import annotations

import time

from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request

from ..auth import decode_bearer_token
from ..request_context import (
    is_cache_hit,
    reset_cache_hit,
    reset_request_context,
    set_request_context,
)
from .emit import emit
from .envelope import uuid7
from .ip_hash import hash_ip
from .payloads import AuthEventPayload, AuthOutcome, HttpAccessPayload

REQUEST_ID_HEADER = "X-Request-ID"


class AuditMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        request_id = f"req_{uuid7().replace('-', '')[:20]}"
        ip_hash = hash_ip(request.client.host if request.client else None)
        raw_auth = request.headers.get("authorization")
        username = decode_bearer_token(raw_auth)

        reset_cache_hit()
        set_request_context(request_id=request_id, ip_hash=ip_hash, username=username)

        if raw_auth and username is None:
            emit(
                "auth.token_invalid",
                AuthEventPayload(outcome=AuthOutcome.TOKEN_INVALID, ip_hash=ip_hash),
            )

        start = time.time()
        response = await call_next(request)
        duration_ms = (time.time() - start) * 1000

        route = request.scope.get("route")
        route_path = route.path if route is not None else request.url.path

        emit(
            "http.access",
            HttpAccessPayload(
                route=route_path,
                method=request.method,
                status_code=response.status_code,
                duration_ms=duration_ms,
                cache_hit=is_cache_hit(),
                ip_hash=ip_hash,
            ),
        )
        response.headers[REQUEST_ID_HEADER] = request_id
        reset_request_context()
        return response
