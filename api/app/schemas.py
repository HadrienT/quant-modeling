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


class PricingResponse(BaseModel):
    npv: float
    greeks: Greeks
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
