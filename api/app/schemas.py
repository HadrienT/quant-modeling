from enum import Enum
from typing import Optional

from pydantic import BaseModel, Field


class EngineType(str, Enum):
    analytic = "analytic"
    mc = "mc"


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
    engine: EngineType = EngineType.analytic
    n_paths: int = 200000
    seed: int = 1
    mc_epsilon: float = 0.0


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
