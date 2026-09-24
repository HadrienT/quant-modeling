from datetime import date, datetime, timezone
from enum import Enum
from typing import List, Literal, Optional

from pydantic import BaseModel, Field, model_validator


def _today_utc() -> date:
    """Today's date in UTC — matches the C++ Date::today()."""
    return datetime.now(timezone.utc).date()


class EngineType(str, Enum):
    analytic = "analytic"
    mc = "mc"
    binomial = "binomial"
    trinomial = "trinomial"
    pde = "pde"


class AmericanEngineType(str, Enum):
    binomial = "binomial"
    trinomial = "trinomial"


class AsianAverageType(str, Enum):
    arithmetic = "arithmetic"
    geometric = "geometric"


class BarrierKind(str, Enum):
    up_and_in = "up-and-in"
    up_and_out = "up-and-out"
    down_and_in = "down-and-in"
    down_and_out = "down-and-out"


class DigitalPayoffKind(str, Enum):
    cash_or_nothing = "cash-or-nothing"
    asset_or_nothing = "asset-or-nothing"


class LookbackStyle(str, Enum):
    fixed_strike = "fixed-strike"
    floating_strike = "floating-strike"


class LookbackExtremum(str, Enum):
    minimum = "minimum"
    maximum = "maximum"


class VanillaRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    is_american: bool = False
    engine: EngineType = EngineType.analytic
    n_paths: int = 200000
    seed: int = 1
    mc_epsilon: float = 0.0
    tree_steps: int = Field(100, ge=10)
    pde_space_steps: int = Field(100, ge=10)
    pde_time_steps: int = Field(100, ge=10)


class AmericanVanillaRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    engine: AmericanEngineType = AmericanEngineType.binomial
    tree_steps: int = Field(100, ge=10)
    pde_space_steps: int = Field(100, ge=10)
    pde_time_steps: int = Field(100, ge=10)


class AsianRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    average_type: AsianAverageType = AsianAverageType.arithmetic
    engine: EngineType = EngineType.analytic
    n_paths: int = 200000
    seed: int = 1
    mc_epsilon: float = 0.0


class DatedAsianRequest(BaseModel):
    """Average-price Asian priced from calendar fixing dates through the
    timeline / day-count architecture (distinct from the legacy AsianRequest,
    which takes a single float maturity)."""

    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    valuation_date: date = Field(
        default_factory=_today_utc,
        description="Time 0. Defaults to today (UTC) when omitted.",
    )
    fixing_dates: List[date] = Field(..., min_length=1)
    strike: float = Field(..., gt=0.0)
    is_call: bool = True
    geometric: bool = False
    day_count: Literal["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] = "ACT/365F"
    sampler: Literal["pseudo", "sobol"] = "pseudo"
    n_paths: int = Field(200_000, ge=1_000, le=5_000_000)
    seed: int = 1


class ScriptRequest(BaseModel):
    """Price a payoff described in text (blueprint/wp/16-scripting.md) — a
    single underlying reachable as `spot()`, priced by the generic Monte-Carlo
    engine. `fuzzy` smooths comparisons for a usable pathwise delta on
    digitals and barriers; discrete tests (flags) stay crisp either way.

    A script only describes a payoff; the dynamics its price depends on come
    from `model`. "black_scholes" takes `spot`, `vol` (one flat volatility).
    "local_vol" takes a `ticker` and prices against the stored market data in
    the database data-ingest fills (option-chain snapshot, close, dividend
    yield) -- never a live source -- so skew is priced. A stored snapshot is a
    market date: the script is priced on the latest snapshot on or before
    `valuation_date` (at most a few days earlier), and the response says so.
    Missing or stale stored data is an error naming what to refresh. Either
    way, `warnings` in the response reports what the script's price depends
    on that the chosen model cannot capture."""

    script: str = Field(..., min_length=1, description="The script source text.")
    model: Literal["black_scholes", "local_vol"] = Field(
        "black_scholes",
        description=(
            "'black_scholes': flat vol (needs spot and vol). 'local_vol': "
            "Dupire surface calibrated from the ticker's stored option-chain "
            "snapshot (needs ticker; spot and dividend come from the database)."
        ),
    )
    ticker: Optional[str] = Field(
        None,
        min_length=1,
        description="Underlying whose stored option chain is calibrated (model='local_vol'); must be in data-ingest's tracked universe.",
    )
    steps_per_year: int = Field(
        52,
        ge=12,
        le=504,
        description="Euler steps per year for model='local_vol' (ignored by black_scholes, which is simulated exactly).",
    )
    spot: Optional[float] = Field(
        None, gt=0.0, description="Required for model='black_scholes'."
    )
    rate: float
    dividend: float = 0.0
    vol: Optional[float] = Field(
        None, gt=0.0, description="Required for model='black_scholes'."
    )
    valuation_date: date = Field(
        default_factory=_today_utc,
        description="Time 0. Defaults to today (UTC) when omitted.",
    )
    day_count: Literal["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] = "ACT/365F"
    fuzzy: bool = False
    default_eps: float = Field(0.01, gt=0.0)
    sampler: Literal["pseudo", "sobol"] = "pseudo"
    n_paths: int = Field(200_000, ge=1_000, le=5_000_000)
    seed: int = 1
    greeks_method: Literal["none", "aad"] = Field(
        "none",
        description=(
            "'aad': every model parameter's sensitivity (spot, rate, div, vol) "
            "from one adjoint Monte-Carlo run (blueprint/wp/17-aad.md), at "
            "roughly 3-5x the cost of the price alone rather than a bumped "
            "reprice per parameter. 'bump' is not offered for scripted "
            "payoffs -- only 'none' or 'aad'. Ignored together with "
            "sampler='sobol': the adjoint engine does not have Sobol support "
            "yet (lot 17d) and falls back to pseudo-random, noted in the "
            "response's diagnostics."
        ),
    )

    @model_validator(mode="after")
    def _model_inputs_present(self) -> "ScriptRequest":
        if self.model == "black_scholes":
            if self.spot is None or self.vol is None:
                raise ValueError("model='black_scholes' requires both spot and vol")
        elif self.ticker is None:
            raise ValueError("model='local_vol' requires a ticker")
        return self


class ScriptValidateRequest(BaseModel):
    """Parse a script and resolve its timeline, without pricing it — no
    market inputs needed. For an editor's "Validate" action: instant, and
    surfaces a malformed script before a 200k-path simulation is even
    considered."""

    script: str = Field(..., min_length=1, description="The script source text.")
    valuation_date: date = Field(
        default_factory=_today_utc,
        description="Time 0. Defaults to today (UTC) when omitted.",
    )
    day_count: Literal["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] = "ACT/365F"


class ScriptEvent(BaseModel):
    date: date
    t: float = Field(..., description="Year-fraction from the valuation date.")


class ScriptAnalysis(BaseModel):
    """Structural facts read off the script that decide which model dynamics
    its price depends on -- never a numerical estimate."""

    n_underlyings: int
    nonlinear_in_spot: bool
    spot_threshold_test: bool
    path_dependent: bool


class ScriptValidateResponse(BaseModel):
    events: List[ScriptEvent]
    variables: List[str]
    analysis: ScriptAnalysis


class BarrierRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    barrier_level: float = Field(..., gt=0.0)
    barrier_kind: BarrierKind
    rebate: float = 0.0
    # Barrier MC runs 9 path-variants per path (CRN Greek scheme);
    # 50 000 paths keeps latency under ~2 s.  Raise for more precision.
    n_paths: int = 50000
    seed: int = 1
    mc_epsilon: float = 0.0
    # 0 = auto (50 steps/yr).  Pass > 0 to override.
    n_steps: int = 0
    brownian_bridge: bool = True


class DigitalRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    payoff_type: DigitalPayoffKind = DigitalPayoffKind.cash_or_nothing
    cash_amount: float = Field(1.0, gt=0.0)


class LookbackRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    style: LookbackStyle = LookbackStyle.fixed_strike
    # extremum is only relevant for fixed-strike; ignored for floating-strike.
    extremum: LookbackExtremum = LookbackExtremum.maximum
    n_steps: int = 0  # 0 = auto (252 × T steps)
    n_paths: int = 200000
    seed: int = 1
    mc_antithetic: bool = True
    mc_epsilon: float = 0.0


class BasketRequest(BaseModel):
    # Per-asset parameters (all lists must have the same length n ≥ 2)
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    weights: List[float] = Field(default_factory=list)
    # Uniform pairwise correlation ρ ∈ (-1, 1).
    # The full n×n correlation matrix is C[i][j] = ρ for i≠j, 1 for i=j.
    pairwise_correlation: float = Field(0.0, ge=-0.999, le=0.999)
    # Basket-level parameters
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    is_call: bool
    n_paths: int = 200000
    seed: int = 1
    mc_antithetic: bool = True


class FutureRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    notional: float = 1.0


class ZeroCouponBondRequest(BaseModel):
    maturity: float = Field(..., gt=0.0)
    rate: float
    notional: float = 1.0
    discount_times: List[float] = Field(default_factory=list)
    discount_factors: List[float] = Field(default_factory=list)


class FixedRateBondRequest(BaseModel):
    maturity: float = Field(..., gt=0.0)
    rate: float
    coupon_rate: float = Field(..., ge=0.0)
    coupon_frequency: int = Field(1, ge=1)
    notional: float = 1.0
    discount_times: List[float] = Field(default_factory=list)
    discount_factors: List[float] = Field(default_factory=list)


class Greeks(BaseModel):
    delta: Optional[float] = None
    gamma: Optional[float] = None
    vega: Optional[float] = None
    theta: Optional[float] = None
    rho: Optional[float] = None

    delta_std_error: Optional[float] = None
    gamma_std_error: Optional[float] = None
    vega_std_error: Optional[float] = None
    theta_std_error: Optional[float] = None
    rho_std_error: Optional[float] = None


class BondAnalytics(BaseModel):
    macaulay_duration: Optional[float] = None
    modified_duration: Optional[float] = None
    convexity: Optional[float] = None
    dv01: Optional[float] = None


class RiskEntry(BaseModel):
    """One model parameter's AAD sensitivity (blueprint/wp/17-aad.md §13.1).
    Present only when priced with greeks_method="aad" -- unlike Greeks'
    five fixed slots, a future model with a local-vol grid or a multi-asset
    correlation matrix reports here regardless of how many parameters it
    has."""

    label: str
    value: float
    std_error: float


class ModelWarning(BaseModel):
    """Something the script's price depends on that the chosen model cannot
    capture (scripting/model_advice.hpp)."""

    code: str
    severity: Literal["warning", "info"]
    message: str


class PricingResponse(BaseModel):
    npv: float
    greeks: Greeks
    bond_analytics: Optional[BondAnalytics] = None
    diagnostics: str
    mc_std_error: float
    risks: Optional[List[RiskEntry]] = None
    warnings: List[ModelWarning] = Field(default_factory=list)
    compute_ms: Optional[float] = Field(
        None,
        description="Server-side wall time of the pricing itself (the engine "
        "call), in milliseconds; excludes network and request parsing.",
    )


class MarketHistoryPoint(BaseModel):
    date: date
    close: float


class MarketHistoryResponse(BaseModel):
    ticker: str
    currency: str = Field(
        "USD", description="ISO currency of the closes (never converted)."
    )
    points: List[MarketHistoryPoint]


class RatePoint(BaseModel):
    tenor: float = Field(description="Years.")
    rate: float = Field(description="Decimal (0.0397 = 3.97 %).")


class QuotedRatePoint(BaseModel):
    tenor: float
    label: str
    series_id: str
    rate: float


class GovernmentCurveResponse(BaseModel):
    name: str
    source: str
    source_url: str
    quote: Literal["par_semiannual", "zero_continuous"]
    as_of: date
    quoted: List[QuotedRatePoint]
    zero: Optional[List[RatePoint]] = Field(
        description="Continuously compounded zero rates, sampled between the first "
        "and last pillar; None when not derivable (see no_derivation)."
    )
    forward: Optional[List[RatePoint]] = Field(
        description="Continuously compounded forwards over forward_period_years, by start tenor."
    )
    forward_period_years: float
    no_derivation: Optional[str] = None


class BenchmarkRate(BaseModel):
    series_id: str
    label: str
    kind: Literal["overnight", "policy", "compounded", "interbank_monthly"]
    backward_looking: bool
    rate: Optional[float]
    as_of: Optional[date]


class MethodologySection(BaseModel):
    title: str
    paragraphs: List[str]


class RatesOverviewResponse(BaseModel):
    currency: Literal["USD", "EUR", "GBP", "CHF", "JPY"]
    unit: Literal["decimal"] = "decimal"
    government: Optional[GovernmentCurveResponse]
    government_unavailable: Optional[str] = None
    benchmarks: List[BenchmarkRate]
    headline_series: str
    methodology: List[MethodologySection]
    warnings: List[str]


FxCurrency = Literal["USD", "EUR", "GBP", "JPY", "CHF"]


class FxForwardPoint(BaseModel):
    label: str
    tenor: float
    forward: float = Field(description="QUOTE units per BASE.")
    points: float = Field(description="Forward − spot, QUOTE units.")


class FxOverviewResponse(BaseModel):
    base: FxCurrency
    quote: FxCurrency
    spot: float = Field(description="QUOTE units per BASE (ECB reference rates).")
    spot_date: date
    forwards: Optional[List[FxForwardPoint]] = None
    forwards_unavailable: Optional[str] = None
    curve_dates: dict[str, date] = {}
    realised_vol: dict[str, Optional[float]] = Field(
        description="Annualised volatility of daily log returns, by window (1Y, 3Y, 5Y)."
    )
    methodology: List["MethodologySection"]
    warnings: List[str]


class FxHistoryPoint(BaseModel):
    date: date
    rate: float


class FxHistoryResponse(BaseModel):
    base: FxCurrency
    quote: FxCurrency
    points: List[FxHistoryPoint]


class FxCorrelationResponse(BaseModel):
    ticker: str
    base: FxCurrency
    quote: FxCurrency
    window: Literal["1Y", "3Y", "5Y"]
    frequency: Literal["weekly", "daily"]
    correlation: float
    n: int = Field(description="Number of common returns.")
    ci_low: float = Field(description="95 % confidence interval (Fisher transform).")
    ci_high: float
    asset_vol: float
    fx_vol: float
    start: date
    end: date
    asset_last: float = Field(description="Asset close on the last common date.")
    fx_last: float = Field(description="FX rate on the last common date.")


class RateHistoryPoint(BaseModel):
    date: date
    rate: float


class RatesHistoryResponse(BaseModel):
    currency: str
    series_id: str
    points: List[RateHistoryPoint]


# ---------------------------------------------------------------------------
# Local-volatility pricing
# ---------------------------------------------------------------------------


class LocalVolRequest(BaseModel):
    """Query parameters for the Dupire local-vol pricing endpoint."""

    ticker: str = Field(..., description="Stock ticker, e.g. 'AAPL'")
    strike: float = Field(..., gt=0)
    maturity: float = Field(..., gt=0, description="Time to maturity in years")
    is_call: bool = True
    rate: float = Field(0.05, description="Risk-free rate (continuous)")
    n_paths: int = Field(50_000, ge=1000, le=500_000)
    n_steps_per_year: int = Field(252, ge=12, le=2520)
    seed: int = 1
    compute_greeks: bool = True
    # Cleaning thresholds
    min_open_interest: int = Field(10, ge=1)
    min_bid: float = Field(0.05, ge=0.0)
    max_spread_ratio: float = Field(0.50, gt=0.0, le=1.0)
    min_moneyness: float = Field(0.70, gt=0.0)
    max_moneyness: float = Field(1.40, gt=0.0)


# ---------------------------------------------------------------------------
# Autocall
# ---------------------------------------------------------------------------


class AutocallRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    observation_dates: List[float] = Field(..., min_length=1)
    autocall_barrier: float = Field(1.0, gt=0.0)
    coupon_barrier: float = Field(0.8, gt=0.0)
    put_barrier: float = Field(0.6, gt=0.0)
    coupon_rate: float = Field(0.05, ge=0.0)
    notional: float = 1000.0
    memory_coupon: bool = True
    ki_continuous: bool = False
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Mountain / Himalaya
# ---------------------------------------------------------------------------


class MountainRequest(BaseModel):
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    correlations: List[List[float]] = Field(default_factory=list)
    observation_dates: List[float] = Field(..., min_length=1)
    strike: float = 0.0
    is_call: bool = True
    rate: float = 0.05
    notional: float = 100.0
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Variance Swap
# ---------------------------------------------------------------------------


class VarianceSwapRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    strike_var: float = Field(..., ge=0.0, description="Annualised variance strike")
    notional: float = 100.0
    observation_dates: List[float] = Field(default_factory=list)
    engine: EngineType = EngineType.analytic
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Volatility Swap
# ---------------------------------------------------------------------------


class VolatilitySwapRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    strike_vol: float = Field(..., ge=0.0, description="Annualised vol strike")
    notional: float = 100.0
    observation_dates: List[float] = Field(default_factory=list)
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Dispersion Swap
# ---------------------------------------------------------------------------


class DispersionSwapRequest(BaseModel):
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    weights: List[float] = Field(default_factory=list)
    pairwise_correlation: float = Field(0.0, ge=-0.999, le=0.999)
    maturity: float = Field(..., gt=0.0)
    strike_spread: float = 0.0
    rate: float = 0.05
    notional: float = 100.0
    observation_dates: List[float] = Field(default_factory=list)
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# FX Forward
# ---------------------------------------------------------------------------


class FXForwardRequest(BaseModel):
    spot: float = Field(..., gt=0.0, description="Spot FX rate (domestic per foreign)")
    rate_domestic: float
    rate_foreign: float
    vol: float = Field(0.1, ge=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    notional: float = 1.0


# ---------------------------------------------------------------------------
# FX Option
# ---------------------------------------------------------------------------


class FXOptionRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate_domestic: float
    rate_foreign: float
    vol: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    is_call: bool = True
    notional: float = 1.0


class QuantoRequest(BaseModel):
    """A European option on a foreign-currency asset, paid in the domestic
    currency at a conversion rate fixed in advance (Reiner 1992). Rates and
    vols are decimals; `correlation` is corr(asset, FX) with the FX rate
    quoted domestic per foreign (a EUR asset paid in USD: EUR/USD) — the
    /market/fx/correlation endpoint estimates it."""

    spot: float = Field(
        ..., gt=0.0, description="Asset, in its own (foreign) currency."
    )
    strike: float = Field(..., gt=0.0, description="In the foreign currency.")
    maturity: float = Field(..., gt=0.0)
    rate_domestic: float = Field(description="Payment currency's rate.")
    rate_foreign: float = Field(description="Asset currency's rate.")
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0, description="Asset volatility σ_S.")
    fx_vol: float = Field(..., gt=0.0, description="FX volatility σ_X.")
    correlation: float = Field(..., ge=-1.0, le=1.0)
    fx_rate: float = Field(
        1.0, gt=0.0, description="Fixed conversion rate, domestic per foreign."
    )
    is_call: bool = True
    engine: Literal["analytic", "mc"] = "analytic"
    n_paths: int = Field(200_000, ge=1_000, le=5_000_000)
    seed: int = 1


# ---------------------------------------------------------------------------
# Commodity Forward
# ---------------------------------------------------------------------------


class CommodityForwardRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    storage_cost: float = 0.0
    convenience_yield: float = 0.0
    vol: float = Field(0.2, ge=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    notional: float = 1.0


# ---------------------------------------------------------------------------
# Commodity Option
# ---------------------------------------------------------------------------


class CommodityOptionRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    storage_cost: float = 0.0
    convenience_yield: float = 0.0
    vol: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    is_call: bool = True
    notional: float = 1.0


# ---------------------------------------------------------------------------
# Rainbow (worst-of / best-of)
# ---------------------------------------------------------------------------


class RainbowKind(str, Enum):
    worst_of = "worst-of"
    best_of = "best-of"


class RainbowRequest(BaseModel):
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    pairwise_correlation: float = Field(0.0, ge=-0.999, le=0.999)
    maturity: float = Field(..., gt=0.0)
    strike: float = Field(1.0, description="Performance strike (1.0 = ATM)")
    is_call: bool = True
    rate: float = 0.05
    notional: float = 100.0
    rainbow_kind: RainbowKind = RainbowKind.worst_of
    n_paths: int = 200000
    seed: int = 1


class LocalVolResponse(BaseModel):
    """Pricing result from the Dupire local-vol MC engine."""

    ticker: str
    spot: float
    npv: float
    mc_std_error: float
    delta: Optional[float] = None
    gamma: Optional[float] = None
    theta: Optional[float] = None
    rho: Optional[float] = None
    vega_parallel: Optional[float] = None
    n_clean_quotes: int
    cleaning_summary: str
    diagnostics: str


class CleanedIVSurfaceResponse(BaseModel):
    """Cleaned & smoothed IV surface grid (bicubic spline evaluated on a regular mesh)."""

    ticker: str
    spot: float
    strikes: List[float]
    maturities: List[float]
    values: List[List[Optional[float]]]
    n_clean_quotes: int
    cleaning_summary: str


class DeltaBucketRow(BaseModel):
    """One maturity's desk-style delta-bucketed smile: 10/25-delta put, ATM,
    25/10-delta call, plus the risk reversals and butterflies a desk
    actually quotes skew and convexity as."""

    ttm: float
    tenor_label: str
    vol_10p: Optional[float] = None
    vol_25p: Optional[float] = None
    vol_atm: Optional[float] = None
    vol_25c: Optional[float] = None
    vol_10c: Optional[float] = None
    rr25: Optional[float] = None
    bf25: Optional[float] = None
    rr10: Optional[float] = None
    bf10: Optional[float] = None


class DeltaSurfaceResponse(BaseModel):
    ticker: str
    spot: float
    rows: List[DeltaBucketRow]
    n_clean_quotes: int
    cleaning_summary: str


class LocalVolSurfaceResponse(BaseModel):
    """Dupire local-volatility surface grid."""

    ticker: str
    spot: float
    strikes: List[float]
    maturities: List[float]
    values: List[List[Optional[float]]]
    n_clean_quotes: int
    cleaning_summary: str


class SimulationModel(str, Enum):
    black_scholes = "black_scholes"
    sabr = "sabr"


class BSPathRequest(BaseModel):
    spot: float = Field(gt=0)
    rate: float = 0.05
    dividend: float = 0.0
    vol: float = Field(gt=0)
    ttm: float = Field(gt=0)
    n_steps: int = Field(100, ge=2, le=1000)
    n_paths: int = Field(30, ge=1, le=500)
    seed: int = 1


class SABRPathRequest(BaseModel):
    forward: float = Field(gt=0)
    alpha: float = Field(gt=0)
    beta: float = Field(0.5, ge=0, le=1)
    rho: float = Field(ge=-1, le=1)
    nu: float = Field(gt=0)
    ttm: float = Field(gt=0)
    n_steps: int = Field(100, ge=2, le=1000)
    n_paths: int = Field(30, ge=1, le=500)
    seed: int = 1


class SimulationPathsResponse(BaseModel):
    model: SimulationModel
    time_grid: List[float]
    paths: List[List[float]]


class SimulationCalibrateRequest(BaseModel):
    ticker: str
    model: SimulationModel
    ttm: float = Field(
        gt=0,
        description="Target maturity in years -- the nearest calibrated slice is used",
    )
    rate: float = 0.05
    beta: float = Field(
        0.5,
        ge=0,
        le=1,
        description="SABR beta, fixed rather than calibrated (see SABRParams)",
    )


class SimulationCalibrateResponse(BaseModel):
    ticker: str
    model: SimulationModel
    spot: float
    dividend: float
    forward: float
    ttm: float
    slice_ttm: float = Field(
        description="The calibrated SVI slice's own maturity, closest to the requested ttm"
    )
    vol: Optional[float] = None
    alpha: Optional[float] = None
    beta: Optional[float] = None
    rho: Optional[float] = None
    nu: Optional[float] = None
    rmse: Optional[float] = None
    converged: Optional[bool] = None
    n_clean_quotes: int
    cleaning_summary: str
