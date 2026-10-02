"""Request and response models of the xVA endpoint (xva.py). Rates, spreads
and vols are decimals; times in years; money in the portfolio's currency, with
the cash-flow sign convention: a cost to the bank is negative."""

from __future__ import annotations

from datetime import date
from typing import List, Literal, Optional

from pydantic import BaseModel, Field

from .schemas import MethodologySection

Rating = Literal["AAA", "AA", "A", "BBB", "BB", "B", "CCC"]
PortfolioId = Literal[
    "single_swap",
    "bought_swaption",
    "sold_swaption",
    "directional",
    "balanced",
    "bermudan",
    "cancellable",
]

# ── Request ──────────────────────────────────────────────────────────────────


class CsaInput(BaseModel):
    """Terms of the collateral agreement. Thresholds and the minimum transfer
    amount are in currency units."""

    threshold_counterparty: float = Field(default=0.0, ge=0)
    threshold_bank: float = Field(default=0.0, ge=0)
    minimum_transfer_amount: float = Field(default=0.0, ge=0)
    margin_period_of_risk_days: int = Field(
        default=10, ge=1, le=60, description="Business days"
    )
    cashflows: Literal["paid", "withheld", "only_bank_pays"] = "paid"


class HistoricalInput(BaseModel):
    """The real-world dynamics of the short rate, dr = a (θ − r) dt + σ dW.
    Any field left out takes the value estimated on the stored history."""

    mean_reversion: Optional[float] = Field(default=None, gt=0, le=5)
    long_run_rate: Optional[float] = Field(default=None, ge=-0.02, le=0.2)
    sigma: Optional[float] = Field(default=None, gt=0, le=0.05)


class XvaRequest(BaseModel):
    portfolio: PortfolioId = "single_swap"
    counterparty_rating: Rating = "BBB"
    own_rating: Rating = "A"
    recovery: float = Field(default=0.4, ge=0.0, le=0.9)
    csa: Optional[CsaInput] = Field(
        default=None, description="None for an uncollateralised netting set"
    )
    borrowing_spread: float = Field(default=0.0, ge=0, le=0.1)
    lending_spread: float = Field(default=0.0, ge=0, le=0.1)
    historical: HistoricalInput = Field(default_factory=HistoricalInput)
    paths: int = Field(default=10000, ge=1000, le=50000)
    seed: int = Field(default=42, ge=0, le=2**31 - 1)
    pfe_confidence: float = Field(default=0.95, gt=0.5, lt=1.0)


# ── Response ─────────────────────────────────────────────────────────────────


class XvaTrade(BaseModel):
    description: str
    kind: Literal["swap", "swaption", "bermudan"]
    payer: bool
    quantity: float = Field(description="-1 for a sold option")
    notional: float
    fixed_rate: float
    start: float = Field(
        description="Start of a swap, expiry of a swaption, first exercise date "
        "of a Bermudan (then exercisable each year)"
    )
    tenor: float
    value_today: float
    standalone_cva: float
    incremental_cva: float
    marginal_cva: Optional[float] = Field(
        description="Euler share of the CVA; none under a CSA"
    )


class Estimate(BaseModel):
    value: float
    error: float = Field(description="Monte-Carlo standard error")


class ExposureProfileOut(BaseModel):
    times: List[float]
    ee: List[float]
    ene: List[float]
    pfe: List[float]
    epe: float
    eepe: float


class PricingExposure(ExposureProfileOut):
    """Risk-neutral scenarios: what the adjustments integrate."""

    discounted_ee: List[float]
    discounted_ene: List[float]
    discounted_ee_error: List[float]


class Adjustments(BaseModel):
    cva: Estimate
    dva: Estimate
    cva_unilateral: float
    fca: float
    fba: float
    cva_rule_of_thumb: float = Field(description="−spread × EPE × T")


class CreditInput(BaseModel):
    rating: str
    spread: float
    hazard: float
    as_of: date


class HullWhiteInput(BaseModel):
    mean_reversion: float
    sigma: float
    rmse_bp: float
    vol_points: int
    swaption_trades: int


class HistoricalDynamics(BaseModel):
    mean_reversion: float
    long_run_rate: float
    sigma: float
    #: The estimate on the stored series, whatever the request overrode.
    estimated_mean_reversion: Optional[float]
    estimated_long_run_rate: Optional[float]
    estimated_sigma: float
    mean_reversion_std_error: Optional[float]
    long_run_rate_std_error: Optional[float]
    series: str
    since: date
    observations: int
    overridden: List[str]


class XvaMarket(BaseModel):
    currency: str
    curve_as_of: date
    curve_label: str
    hull_white: HullWhiteInput
    counterparty: CreditInput
    own: CreditInput
    recovery: float
    historical: HistoricalDynamics


class XvaResponse(BaseModel):
    portfolio: PortfolioId
    portfolio_label: str
    lesson: str
    value_today: float
    trades: List[XvaTrade]
    #: Exposure after collateral when there is a CSA.
    exposure: PricingExposure
    #: The same netting set on the same paths without the CSA (CSA only).
    exposure_uncollateralised: Optional[PricingExposure]
    #: Real-world scenarios: the risk measures (PFE, EPE, EEPE).
    risk: ExposureProfileOut
    adjustments: Adjustments
    adjustments_uncollateralised: Optional[Adjustments]
    market: XvaMarket
    paths: int
    pilot_paths: int = Field(
        description="Independent paths the regression of a Bermudan was fitted "
        "on; 0 when every trade has a closed form"
    )
    seed: int
    compute_ms: float
    warnings: List[str]
    methodology: List[MethodologySection]


class XvaPortfolioInfo(BaseModel):
    id: PortfolioId
    label: str
    lesson: str


class XvaPortfoliosResponse(BaseModel):
    portfolios: List[XvaPortfolioInfo]
    ratings: List[str]
