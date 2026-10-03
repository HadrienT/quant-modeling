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
    "scripted_swap",
    "custom",
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
    initial_margin_model: Literal["auto", "simm", "regression"] = Field(
        default="auto",
        description="simm: ISDA SIMM from the sensitivities of each scenario "
        "(swaps and European swaptions only). regression: a model of the margin, "
        "fitted on the simulated values. auto: SIMM when every trade can give "
        "its sensitivities, the regression otherwise",
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


class ScriptTradeInput(BaseModel):
    """A trade written in the payoff language of the Scripting page, on
    interest rates only: its events are dated, read the curve with df(DATE)
    and pay with `pays`."""

    script: str = Field(min_length=1, max_length=4000)
    quantity: float = Field(
        default=1.0, ge=-1000, le=1000, description="Negative for the other side"
    )
    label: str = Field(default="Scripted trade", min_length=1, max_length=80)


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
    method: Literal["auto", "sa_ccr", "internal_model"] = Field(
        default="auto",
        description="How the exposure at default is measured. sa_ccr: the "
        "standardised approach, from a supervisory description of each trade. "
        "internal_model: 1.4 × the Effective EPE of the simulation, which needs "
        "no description. auto: sa_ccr unless a trade is a script, which has none",
    )
    pd: Optional[float] = Field(
        default=None,
        gt=0,
        lt=1,
        description="One-year probability of default for the IRB formula; unset "
        "takes the historical default rate of the counterparty's rating",
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
    scripts: List[ScriptTradeInput] = Field(
        default_factory=list,
        max_length=3,
        description="Scripted trades added to the portfolio's; with the "
        "portfolio `custom` they are the whole netting set",
    )
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
    device: Literal["auto", "cpu", "gpu"] = Field(
        default="auto",
        description="Where the paths are valued. auto: the GPU when the server "
        "has one and every trade has a closed form, the CPU otherwise; the "
        "response says which and why. gpu refuses to fall back",
    )


# ── Response ─────────────────────────────────────────────────────────────────


class XvaTrade(BaseModel):
    description: str
    kind: Literal["swap", "swaption", "bermudan", "script"]
    quantity: float = Field(description="-1 for a sold option")
    #: The terms of a swap or a swaption; a script has only its text.
    payer: Optional[bool]
    notional: Optional[float]
    fixed_rate: Optional[float]
    start: Optional[float] = Field(
        description="Start of a swap, expiry of a swaption, first exercise date "
        "of a Bermudan (then exercisable each year)"
    )
    tenor: Optional[float]
    script: Optional[str] = Field(description="The text of a scripted trade")
    maturity: float = Field(description="Years to the trade's last payment")
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
    quantile_levels: List[float] = Field(
        description="Levels of the quantiles of the netting set's value"
    )
    value_quantiles: List[List[float]] = Field(
        description="value_quantiles[k][i]: the quantile quantile_levels[k] of "
        "the value at times[i] — the distribution the profiles summarise"
    )


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


class SimmToday(BaseModel):
    """Today's ISDA SIMM of the netting set, by margin type."""

    delta: float
    vega: float
    curvature: float
    total: float


class InitialMarginOut(BaseModel):
    """The initial margin each party posts, on the dates of the exposure."""

    today: float
    times: List[float]
    expected: List[float]
    requested: Literal["auto", "simm", "regression"]
    model: Literal["simm", "regression"] = Field(
        description="What the margin of each scenario is computed with"
    )
    reason: str = Field(description="Why this model, in plain terms")
    simm_today: Optional[SimmToday] = Field(
        description="Today's SIMM, when the margin is SIMM"
    )


class CapitalOut(BaseModel):
    """Regulatory capital of the netting set: today's, and projected."""

    method: Literal["sa_ccr", "internal_model"] = Field(
        description="How the exposure at default is measured"
    )
    method_reason: str = Field(description="Why this method, in plain terms")
    ead_today: float = Field(description="Exposure at default")
    default_capital_today: float = Field(description="IRB capital on that EAD")
    cva_capital_today: float = Field(description="BA-CVA capital on that EAD")
    times: List[float]
    expected_ead: List[float]
    discounted_capital: List[float] = Field(
        description="E[D(t) K(t)], both charges: what KVA integrates"
    )
    pd: float = Field(description="After the regulatory floor of 0.05 %")
    pd_source: Literal["historical", "entered"] = Field(
        description="historical: the default rate of the rating; entered: the "
        "request's own"
    )
    pd_reason: str = Field(description="Where the PD comes from, in plain terms")
    lgd: float
    sector: str
    investment_grade: bool
    margined: bool = Field(description="The netting set is under a margin agreement")
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


class HistoricalDefaultRate(BaseModel):
    """The default rate of a rating category on record: the average of the
    yearly rates the agency reported to ESMA (CEREP)."""

    agency: str
    rating: str
    first_year: int
    last_year: int
    years: int
    defaults: int = Field(description="Defaults of the category over those years")
    rate: float = Field(description="Average one-year default rate, a decimal")


class XvaMarket(BaseModel):
    currency: str
    curve_as_of: date
    curve_label: str
    hull_white: HullWhiteInput
    counterparty: CreditInput
    own: CreditInput
    recovery: float
    historical: HistoricalDynamics
    #: What the capital's PD rests on; None when the request gave its own.
    default_rate: Optional[HistoricalDefaultRate] = None


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
    device: Literal["cpu", "gpu"] = Field(
        description="Where the paths were actually valued"
    )
    gpus: int = Field(
        description="GPUs that shared the paths (0 on the CPU); the result is "
        "the same bits for any count"
    )
    device_reason: str = Field(description="Why this device, in plain terms")
    compute_ms: float
    warnings: List[str]
    methodology: List[MethodologySection]


class XvaPortfolioInfo(BaseModel):
    id: PortfolioId
    label: str
    lesson: str


class WrongWayScenario(BaseModel):
    """A value of the wrong-way parameter offered as a button, with the text
    shown when the pointer is on it."""

    id: str
    label: str
    wrong_way_risk: float = Field(description="The request's `wrong_way_risk`")
    explanation: str


class XvaPortfoliosResponse(BaseModel):
    portfolios: List[XvaPortfolioInfo]
    ratings: List[str]
    wrong_way_scenarios: List[WrongWayScenario]


# ── Sensitivities (lot X8) ───────────────────────────────────────────────────


class XvaSensitivitiesRequest(BaseModel):
    """The netting set whose CVA, DVA and FVA are differentiated to every
    market quote. Swaps and European swaptions, with or without variation
    margin: what the adjoint run covers."""

    portfolio: PortfolioId = "single_swap"
    counterparty_rating: Rating = "BBB"
    own_rating: Rating = "A"
    recovery: float = Field(default=0.4, ge=0.0, le=0.9)
    csa: Optional[CsaInput] = Field(
        default=None, description="None for an uncollateralised netting set"
    )
    borrowing_spread: float = Field(default=0.0, ge=0, le=0.1)
    lending_spread: float = Field(default=0.0, ge=0, le=0.1)
    sector: Sector = Field(
        default="other", description="The counterparty's sector, for SA-CVA"
    )
    paths: int = Field(default=5000, ge=1000, le=20000)
    seed: int = Field(default=42, ge=0, le=2**31 - 1)


class AdjustmentRisks(BaseModel):
    """The sensitivity of each adjustment to one quantity, per unit of it
    (multiply by 0.0001 for a basis point), with its Monte-Carlo error."""

    label: str
    expiry: float = Field(description="Expiry of a swaption; 0 otherwise")
    tenor: float = Field(description="Tenor of the swap, the swaption or the spread")
    level: float = Field(description="The quantity today")
    cva: Estimate
    dva: Estimate
    fca: Estimate
    fba: Estimate
    cva_unilateral: Estimate = Field(
        description="The regulatory CVA's: the bank assumed default-free"
    )


class SensitivityAdjustments(BaseModel):
    cva: Estimate
    dva: Estimate
    fca: Estimate
    fba: Estimate
    cva_unilateral: Estimate


class SaCvaOut(BaseModel):
    """The standardised approach for CVA risk (MAR50), from the sensitivities
    of the unilateral CVA."""

    interest_rate_tenors: List[float]
    interest_rate_delta: List[float] = Field(
        description="Sensitivity to the risk-free yield of each tenor, per unit"
    )
    interest_rate_risk_weights: List[float]
    interest_rate_vega: float = Field(
        description="Sensitivity to a relative shift of every volatility, per unit"
    )
    credit_spread_tenors: List[float]
    credit_spread_delta: List[float]
    credit_spread_risk_weight: float
    capital_interest_rate_delta: float
    capital_interest_rate_vega: float
    capital_credit_spread_delta: float
    capital: float
    sector: str
    investment_grade: bool


class XvaSensitivitiesResponse(BaseModel):
    portfolio: PortfolioId
    portfolio_label: str
    adjustments: SensitivityAdjustments
    #: To what the market quotes.
    swap_rates: List[AdjustmentRisks]
    swaption_vols: List[AdjustmentRisks]
    counterparty_spreads: List[AdjustmentRisks]
    own_credit: List[AdjustmentRisks]
    others: List[AdjustmentRisks] = Field(
        description="Losses given default and funding spreads"
    )
    #: To what the model is written in.
    model_risks: List[AdjustmentRisks]
    sa_cva: SaCvaOut
    hull_white: HullWhiteInput
    market_as_of: date
    paths: int
    seed: int
    threads: int
    inputs: int = Field(description="Inputs of the model differentiated to")
    seconds_adjoint: float = Field(
        description="Wall time of the paths with every sensitivity"
    )
    seconds_valuation: float = Field(
        description="Wall time of the same paths without sensitivities"
    )
    cost_ratio: float = Field(description="seconds_adjoint / seconds_valuation")
    bump_valuations: int = Field(
        description="Valuations a central difference of every input would take"
    )
    compute_ms: float
    warnings: List[str]
    methodology: List[MethodologySection]
