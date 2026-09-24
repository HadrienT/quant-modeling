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
from typing import Any

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


# ── pricing.valuation (WP §6, lot 18e) ───────────────────────────────────────


class MarketInputStatus(str, Enum):
    """The desks' vocabulary for the quality of a market input (WP §6): a
    price computed on a `default` input is not forbidden, it is labelled."""

    OBSERVED = "observed"  # read at the source, and fresh
    STALE = "stale"  # read, but older than its freshness rule allows
    PROXIED = "proxied"  # replaced by a neighbouring input
    DEFAULT = "default"  # a fallback constant


class MarketInput(BaseModel):
    """One market datum a valuation read from the database. Only its hash is
    kept: enough for a replay to tell whether the data was revised since
    (`drifted_inputs`), without copying the market database into the audit.
    Inputs the caller typed in (spot, vol… of a manual pricing) are not market
    inputs: they are in `request`, verbatim."""

    name: str  # e.g. "spot:SPY", "dividend:SPY", "option_chain:SPY"
    source: str  # e.g. "db:prices.sp500_daily"
    as_of: str | None
    status: MarketInputStatus
    value_hash: str


class ModelSpec(BaseModel):
    name: str
    params: dict[str, str | int | float | None] = {}
    calibration_id: str | None = None


class EngineSpec(BaseModel):
    name: str
    n_paths: int | None = None
    seed: int | None = None
    scheme: str | None = None


class ValuationResult(BaseModel):
    npv: float
    mc_std_error: float
    greeks: dict[str, float | None]


class ValuationTiming(BaseModel):
    duration_ms: float


class CodeVersion(BaseModel):
    api_sha: str
    lib_build: str


class ValuationPayload(BaseModel):
    """prix = f(instrument, données de marché, configuration du modèle, version
    du code, graine) (WP §6): with the five recorded, a valuation can be
    replayed (`POST /api/admin/replay/{event_id}`).

    `request` is the pricing request as received; the pricing request models
    carry no credential (asserted by test_audit.py on the models themselves)."""

    product: str
    request: dict[str, Any]
    request_hash: str
    model: ModelSpec
    engine: EngineSpec
    market_inputs: list[MarketInput]
    result: ValuationResult
    timing: ValuationTiming
    code: CodeVersion
    ip_hash: str | None = None


# ── assistant.chat ───────────────────────────────────────────────────────────


class AssistantOutcome(str, Enum):
    COMPLETED = "completed"
    FAILED = "failed"


class AssistantChatPayload(BaseModel):
    """Metadata only — never the prompt nor the answer (WP §7)."""

    outcome: AssistantOutcome
    turns: int
    has_script: bool
    has_error: bool
    duration_ms: float
    scripts_proposed: int
    scripts_valid: int
