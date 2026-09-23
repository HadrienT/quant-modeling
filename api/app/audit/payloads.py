"""Per-event payload models (contract §1, WP §4.2 / §5.1).

Each event type owns a closed Pydantic model. A field that isn't declared here
can never reach the audit trail — that is the "by construction" guarantee
against a password, token, or Authorization header leaking into an event.
`api/tests/test_audit.py` checks this on the models themselves (their declared
fields), not on example instances, exactly as the WP's acceptance criterion
asks for.
"""

from __future__ import annotations

from enum import Enum

from pydantic import BaseModel


class FallbackKind(str, Enum):
    """A closed enumeration (WP §5.1): adding a member means touching this
    file, which is what keeps a fallback from becoming silent again."""

    LIVE_YFINANCE_CHAIN = "live_yfinance_chain"
    LIVE_YFINANCE_SPOT = "live_yfinance_spot"
    LIVE_YFINANCE_DIVIDEND = "live_yfinance_dividend"
    DEFAULT_RATE = "default_rate"
    STALE_DATA = "stale_data"
    PROXIED_INPUT = "proxied_input"


class FallbackPayload(BaseModel):
    kind: FallbackKind
    detail: str = ""
    context: dict[str, str | int | float | None] = {}


class AuthOutcome(str, Enum):
    LOGIN_OK = "login_ok"
    LOGIN_FAILED = "login_failed"
    REGISTER = "register"
    RATE_LIMITED = "rate_limited"
    TOKEN_INVALID = "token_invalid"


class AuthEventPayload(BaseModel):
    outcome: AuthOutcome
    ip_hash: str | None = None


class HttpAccessPayload(BaseModel):
    route: str
    method: str
    status_code: int
    duration_ms: float
    cache_hit: bool
    ip_hash: str | None = None
