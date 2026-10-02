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
    initial_margin: bool = Field(
        default=False,
        description="Both parties also post initial margin, as the margin rules "
        "for non-cleared derivatives require: projected by regression, 99 % of "
        "the move over the margin period of risk",
    )
    initial_margin_today: Optional[float] = Field(
        default=None,
        gt=0,
        description="The initial margin actually computed today (a SIMM amount): "
        "the projected profile is scaled to start from it",
    )
    collateral_rate_spread: float = Field(
        default=0.0,
        ge=-0.05,
        le=0.05,
        description="What the agreement pays on cash collateral, over the rate "
        "the trades are discounted at; 0 for a CSA paying the overnight rate",
    )


class HistoricalInput(BaseModel):
    """The real-world dynamics of the short rate, dr = a (θ − r) dt + σ dW.
    Any field left out takes the value estimated on the stored history."""

    mean_reversion: Optional[float] = Field(default=None, gt=0, le=5)
    long_run_rate: Optional[float] = Field(default=None, ge=-0.02, le=0.2)
    sigma: Optional[float] = Field(default=None, gt=0, le=0.05)


Sector = Literal[
    "sovereign",
    "local_government",
    "financial",
    "basic_materials_energy_industrials",
    "consumer_transport_administrative",
    "technology_telecommunications",
    "health_care_utilities_professional",
    "other",
]


class CapitalInput(BaseModel):
    """What the regulatory capital of the netting set depends on, besides its
    exposure, and what that capital costs."""

    cost_of_capital: float = Field(
        default=0.10,
        ge=0,
        le=0.5,
        description="Return required on the capital held, a year",
    )
    sector: Sector = Field(
        default="other", description="The counterparty's sector, for BA-CVA"
    )
    pd: Optional[float] = Field(
        default=None,
        gt=0,
        lt=1,
        description="One-year probability of default for the IRB formula; unset "
        "takes the one implied by the rating's spread, which is higher than a "
        "bank's own estimate would be",
    )
    lgd: Optional[float] = Field(
        default=None,
        ge=0,
        le=1,
        description="Regulatory loss given default; unset takes the foundation "
        "approach's 45 % for a financial counterparty, 40 % otherwise",
    )


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
    capital: CapitalInput = Field(default_factory=lambda: CapitalInput())
    wrong_way_risk: float = Field(
        default=0.0,
        ge=-30.0,
        le=30.0,
        description="Wrong-way risk (Hull & White 2012): the counterparty's hazard "
        "rate is multiplied by exp(this × change of the netting set's value / 10 M). "
        "0 is independence, > 0 wrong-way, < 0 right-way; 10 means the hazard "
        "rises by about 65 % when the value rises by 5 % of the reference notional",
    )
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
    cva_independent: Estimate = Field(
        description="CVA with the counterparty's default independent of the "
        "exposure; equal to cva unless wrong_way_risk is set"
    )
    dva_independent: Estimate
    cva_unilateral: float
    fca: float
    fba: float
    colva: float = Field(description="Rate paid on the collateral; 0 without a CSA")
    mva: float = Field(description="Funding of the initial margin posted")
    kva: float = Field(description="Cost of the regulatory capital held")
    cva_rule_of_thumb: float = Field(description="−spread × EPE × T")


class InitialMarginOut(BaseModel):
    """The initial margin each party posts, on the dates of the exposure."""

    today: float
    times: List[float]
    expected: List[float]


class CapitalOut(BaseModel):
    """Regulatory capital of the netting set: today's, and projected."""

    ead_today: float = Field(description="SA-CCR exposure at default")
    default_capital_today: float = Field(description="IRB capital on that EAD")
    cva_capital_today: float = Field(description="BA-CVA capital on that EAD")
    times: List[float]
    expected_ead: List[float]
    discounted_capital: List[float] = Field(
        description="E[D(t) K(t)], both charges: what KVA integrates"
    )
    pd: float = Field(description="After the regulatory floor of 0.05 %")
    pd_is_market_implied: bool
    lgd: float
    sector: str
    investment_grade: bool
    margined: bool = Field(description="SA-CCR treats the netting set as margined")
    cost_of_capital: float


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
    initial_margin: Optional[InitialMarginOut]
    capital: CapitalOut
    capital_uncollateralised: Optional[CapitalOut]
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
