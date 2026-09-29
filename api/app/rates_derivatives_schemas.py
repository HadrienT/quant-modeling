"""Request and response models of the /rates page (rates_derivatives.py).
Rates and vols are decimals (0.0123 = 1.23 % = 123 bp); times in years."""

from __future__ import annotations

from typing import List, Optional

from pydantic import BaseModel, Field, field_validator

from .schemas import MethodologySection

# ── Request ──────────────────────────────────────────────────────────────────


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


class RatesAnalysisRequest(BaseModel):
    fixed_frequency: int = Field(default=1, ge=1, le=12)
    float_frequency: int = Field(default=2, ge=1, le=12)
    deposits: List[TenorRate] = Field(default_factory=list, max_length=10)
    ois: List[TenorRate] = Field(min_length=1, max_length=40)
    fras: List[FraInput] = Field(default_factory=list, max_length=20)
    swaps: List[TenorRate] = Field(min_length=1, max_length=40)
    swaption_vols: List[SwaptionVolInput] = Field(min_length=1, max_length=60)
    hull_white_mean_reversion: Optional[float] = Field(
        default=None,
        ge=1e-4,
        le=1.0,
        description="Fix a and fit σ only; None fits both",
    )
    swap: SwapInput
    swaption: SwaptionInput


# ── Response ─────────────────────────────────────────────────────────────────


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


class SwapPeriod(BaseModel):
    start: float
    end: float
    payment: float
    accrual: float
    discount: float
    forward: Optional[float] = None


class SwapResult(BaseModel):
    npv: float
    par_rate: float
    annuity: float
    pv01: float
    fixed_leg: float
    floating_leg: float
    fixed_periods: List[SwapPeriod]
    floating_periods: List[SwapPeriod]


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
    model: str
    price: float
    implied_normal_vol: Optional[float]
    detail: str


class SwaptionResult(BaseModel):
    forward: float
    annuity: float
    strike: float
    prices: List[SwaptionModelPrice]
    bermudan_price: Optional[float]
    bermudan_exercises: List[float]
    switch_premium: Optional[float]


class RatesAnalysisResponse(BaseModel):
    curves: CurvesResult
    swap: SwapResult
    hull_white: HullWhiteCalibrationResult
    swaption: SwaptionResult
    methodology: List[MethodologySection]


class RatesExampleResponse(BaseModel):
    currency: str
    label: str
    request: RatesAnalysisRequest
