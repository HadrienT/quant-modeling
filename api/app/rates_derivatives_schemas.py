"""Request and response models of the rates derivatives (rates_derivatives.py):
the quote sets, the curves built from them, and the swap and swaption priced
on them. Rates and vols are decimals (0.0123 = 1.23 % = 123 bp); times in
years."""

from __future__ import annotations

from datetime import date
from typing import List, Literal, Optional

from pydantic import BaseModel, Field, field_validator, model_validator

from .schemas import MethodologySection, PricingResponse

QuoteSetId = Literal["usd-sofr", "eur-illustrative"]
SwaptionModel = Literal["bachelier", "black", "sabr", "hull_white"]

# ── Quotes ───────────────────────────────────────────────────────────────────


class TenorRate(BaseModel):
    tenor: float = Field(gt=0, le=50)
    rate: float = Field(ge=-0.05, le=0.5)


class FraInput(BaseModel):
    start: float = Field(ge=0, le=30)
    end: float = Field(gt=0, le=31)
    rate: float = Field(ge=-0.05, le=0.5)


class SwaptionVolInput(BaseModel):
    expiry: float = Field(gt=0, le=30)
    tenor: float = Field(gt=0, le=30)
    normal_vol: float = Field(gt=0, le=0.05, description="ATM normal vol, rate units")


class RatesCurveQuotes(BaseModel):
    """What the two curves are bootstrapped from: the OIS curve (deposits and
    par OIS swaps) and the floating index (FRAs and par swaps against it)."""

    fixed_frequency: int = Field(default=1, ge=1, le=12)
    float_frequency: int = Field(default=2, ge=1, le=12)
    deposits: List[TenorRate] = Field(default_factory=list, max_length=10)
    ois: List[TenorRate] = Field(min_length=1, max_length=40)
    fras: List[FraInput] = Field(default_factory=list, max_length=20)
    swaps: List[TenorRate] = Field(min_length=1, max_length=40)


# ── Contracts ────────────────────────────────────────────────────────────────


class SwapInput(BaseModel):
    start: float = Field(ge=0, le=30)
    tenor: float = Field(gt=0, le=40)
    fixed_rate: float = Field(ge=-0.05, le=0.5)
    notional: float = Field(gt=0, le=1e10)
    payer: bool = True


class SabrInput(BaseModel):
    alpha: float = Field(gt=0, le=5)
    beta: float = Field(ge=0, le=1)
    rho: float = Field(gt=-1, lt=1)
    nu: float = Field(ge=0, le=5)


class SwaptionInput(BaseModel):
    expiry: float = Field(gt=0, le=20)
    tenor: float = Field(ge=1, le=30, description="Whole years: annual exercise dates")
    strike: Optional[float] = Field(
        default=None, ge=-0.05, le=0.5, description="None for at the money"
    )
    payer: bool = True
    notional: float = Field(gt=0, le=1e10)
    exercise: Literal["european", "bermudan"] = "european"
    model: Literal["auto", "bachelier", "black", "sabr", "hull_white"] = Field(
        default="auto",
        description="auto: Bachelier on the quoted vol for a European, "
        "Hull-White for a Bermudan",
    )
    normal_vol: Optional[float] = Field(
        default=None,
        gt=0,
        le=0.05,
        description="Bachelier vol; None takes the nearest quoted ATM vol",
    )
    lognormal_vol: Optional[float] = Field(default=None, gt=0, le=3)
    shift: float = Field(default=0.0, ge=0, le=0.1)
    sabr: Optional[SabrInput] = None

    @field_validator("tenor")
    @classmethod
    def _whole_years(cls, v: float) -> float:
        if abs(v - round(v)) > 1e-9:
            raise ValueError("the swaption tenor must be a whole number of years")
        return v

    @model_validator(mode="after")
    def _model_has_its_inputs(self) -> "SwaptionInput":
        if self.exercise == "bermudan" and self.model not in ("auto", "hull_white"):
            raise ValueError(
                "a Bermudan swaption is priced under Hull-White: the other "
                "models describe one swap rate at one date"
            )
        if self.model == "black" and self.lognormal_vol is None:
            raise ValueError("the Black model needs a lognormal vol")
        if self.model == "sabr" and self.sabr is None:
            raise ValueError("the SABR model needs its parameters")
        return self


class SwapPricingRequest(BaseModel):
    curves: RatesCurveQuotes
    swap: SwapInput


class SwaptionPricingRequest(BaseModel):
    curves: RatesCurveQuotes
    swaption_vols: List[SwaptionVolInput] = Field(min_length=1, max_length=60)
    hull_white_mean_reversion: Optional[float] = Field(
        default=None,
        ge=1e-4,
        le=1.0,
        description="Fix a and fit σ only; None fits both",
    )
    swaption: SwaptionInput


# ── Curves ───────────────────────────────────────────────────────────────────


class CurvePoint(BaseModel):
    tenor: float
    ois_zero: float = Field(description="Continuously compounded")
    ois_forward: float = Field(description="Simple, over one index period")
    index_forward: float = Field(description="Simple, over one index period")
    basis_bp: float


class CurvesResult(BaseModel):
    ois_pillars: List[float]
    index_pillars: List[float]
    points: List[CurvePoint]
    max_repricing_error_bp: float = Field(
        description="Worst |par rate − quote| over the input swaps"
    )
    single_curve: bool = Field(
        description="The index curve is the OIS curve (an overnight index): "
        "no basis between them"
    )


class RatesCurvesResponse(BaseModel):
    curves: CurvesResult
    methodology: List[MethodologySection]


# ── Swap ─────────────────────────────────────────────────────────────────────


class SwapPeriod(BaseModel):
    start: float
    end: float
    payment_time: float
    accrual: float
    discount: float = Field(description="OIS discount factor of the payment date")
    rate: float = Field(
        description="The fixed rate, or the index forward over the period"
    )
    present_value: float = Field(
        description="rate × accrual × notional × discount, signed for the holder"
    )


class SwapResult(BaseModel):
    par_rate: float
    annuity: float
    pv01: float
    fixed_leg: float = Field(description="Signed for the holder: paid is negative")
    floating_leg: float = Field(description="Signed for the holder: paid is negative")
    fixed_periods: List[SwapPeriod]
    floating_periods: List[SwapPeriod]


class SwapPricingResponse(PricingResponse):
    swap: SwapResult


# ── Swaption ─────────────────────────────────────────────────────────────────


class CalibrationPoint(BaseModel):
    expiry: float
    tenor: float
    strike: float
    market_vol: float
    model_vol: Optional[float]


class HullWhiteCalibrationResult(BaseModel):
    mean_reversion: float
    sigma: float
    mean_reversion_fixed: bool
    rmse_bp: float
    worst_bp: float
    iterations: int
    converged: bool
    seconds: float
    points: List[CalibrationPoint]


class SwaptionModelPrice(BaseModel):
    key: SwaptionModel
    model: str
    price: float
    implied_normal_vol: Optional[float]
    detail: str


class SwaptionResult(BaseModel):
    exercise: Literal["european", "bermudan"]
    requested: Literal["auto", "bachelier", "black", "sabr", "hull_white"]
    model: SwaptionModel = Field(description="The model the value is under")
    reason: str = Field(description="Why this model, in plain terms")
    forward: float
    annuity: float
    strike: float
    prices: List[SwaptionModelPrice] = Field(
        description="The European swaption under every model that has its inputs"
    )
    bermudan_price: Optional[float]
    bermudan_exercises: List[float]
    switch_premium: Optional[float]
    hull_white: HullWhiteCalibrationResult


class SwaptionPricingResponse(PricingResponse):
    swaption: SwaptionResult


# ── Quote sets ───────────────────────────────────────────────────────────────


class MarketSwapRate(BaseModel):
    tenor: float
    rate: float
    trades: int = Field(description="Swaps traded that day behind the median")


class MarketSwaptionVol(BaseModel):
    expiry: float
    tenor: float
    normal_vol: float = Field(description="Median over the window, rate units")
    low: float = Field(description="Lower quartile")
    high: float = Field(description="Upper quartile")
    trades: int


class RejectedTrades(BaseModel):
    reason: str
    trades: int


class RatesTradeStats(BaseModel):
    """What quotes built from traded swaps and swaptions rest on (DTCC public
    dissemination)."""

    window_start: date
    swap_rates: List[MarketSwapRate]
    swaption_vols: List[MarketSwaptionVol]
    trades_used: int
    rejected: List[RejectedTrades]


class RatesQuoteSetResponse(BaseModel):
    """A set of quotes the rates products are priced on, with the contracts
    the workbench opens on."""

    id: QuoteSetId
    currency: str
    label: str
    source: Literal["market", "manual"]
    as_of: Optional[date] = None
    curves: RatesCurveQuotes
    swaption_vols: List[SwaptionVolInput]
    swap: SwapInput
    swaption: SwaptionInput
    trades: Optional[RatesTradeStats] = None
    methodology: List[MethodologySection]
