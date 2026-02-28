from datetime import date
from enum import Enum
from typing import List, Optional

from pydantic import BaseModel, Field


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
    up_and_in   = "up-and-in"
    up_and_out  = "up-and-out"
    down_and_in = "down-and-in"
    down_and_out = "down-and-out"


class DigitalPayoffKind(str, Enum):
    cash_or_nothing  = "cash-or-nothing"
    asset_or_nothing = "asset-or-nothing"


class LookbackStyle(str, Enum):
    fixed_strike    = "fixed-strike"
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
    n_steps: int = 0          # 0 = auto (252 × T steps)
    n_paths: int = 200000
    seed: int = 1
    mc_antithetic: bool = True
    mc_epsilon: float = 0.0


class BasketRequest(BaseModel):
    # Per-asset parameters (all lists must have the same length n ≥ 2)
    spots: List[float] = Field(..., min_length=2)
    vols: List[float]  = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    weights: List[float]   = Field(default_factory=list)
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


class PricingResponse(BaseModel):
    npv: float
    greeks: Greeks
    bond_analytics: Optional[BondAnalytics] = None
    diagnostics: str
    mc_std_error: float


class MarketHistoryPoint(BaseModel):
    date: date
    close: float


class MarketHistoryResponse(BaseModel):
    ticker: str
    points: List[MarketHistoryPoint]


class IVSurfaceResponse(BaseModel):
    ticker: str
    surface: str
    strikes: List[float]
    maturities: List[float]
    values: List[List[Optional[float]]]


class CurvePointResponse(BaseModel):
    x: float
    y: float


class RatesCurveResponse(BaseModel):
    curve: str
    zero: List[CurvePointResponse]


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


class LocalVolSurfaceResponse(BaseModel):
    """Dupire local-volatility surface grid."""
    ticker: str
    spot: float
    strikes: List[float]
    maturities: List[float]
    values: List[List[Optional[float]]]
    n_clean_quotes: int
    cleaning_summary: str
