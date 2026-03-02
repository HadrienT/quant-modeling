"""Portfolio schemas for the quant desk portfolio manager."""

from datetime import datetime
from enum import Enum
from typing import Any, Dict, List, Optional

from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Product category / type enums
# ---------------------------------------------------------------------------

class ProductCategory(str, Enum):
    vanilla = "vanilla"
    exotic = "exotic"
    fixed_income = "fixed-income"
    structured = "structured"
    volatility = "volatility"
    fx = "fx"
    commodity = "commodity"
    rainbow = "rainbow"


class ProductType(str, Enum):
    # Vanilla
    european_call = "european-call"
    european_put = "european-put"
    american_call = "american-call"
    american_put = "american-put"
    # Exotic
    asian = "asian"
    barrier = "barrier"
    digital = "digital"
    lookback = "lookback"
    basket = "basket"
    # Fixed-income
    zero_coupon_bond = "zero-coupon-bond"
    fixed_rate_bond = "fixed-rate-bond"
    future = "future"
    # Structured
    autocall = "autocall"
    mountain = "mountain"
    # Volatility
    variance_swap = "variance-swap"
    volatility_swap = "volatility-swap"
    dispersion_swap = "dispersion-swap"
    # FX
    fx_forward = "fx-forward"
    fx_option = "fx-option"
    # Commodity
    commodity_forward = "commodity-forward"
    commodity_option = "commodity-option"
    # Rainbow
    rainbow = "rainbow"


# ---------------------------------------------------------------------------
# Position & Portfolio models
# ---------------------------------------------------------------------------

class PositionGreeks(BaseModel):
    delta: Optional[float] = None
    gamma: Optional[float] = None
    vega: Optional[float] = None
    theta: Optional[float] = None
    rho: Optional[float] = None


class PositionResult(BaseModel):
    """Cached pricing result for a position."""
    npv: float = 0.0
    unit_price: float = 0.0
    greeks: PositionGreeks = Field(default_factory=PositionGreeks)
    priced_at: Optional[str] = None
    engine: str = ""
    diagnostics: str = ""
    mc_std_error: float = 0.0


class Position(BaseModel):
    """A single position in a portfolio."""
    id: str
    label: str = "Untitled"
    product_type: ProductType
    category: ProductCategory
    direction: str = "long"  # "long" or "short"
    quantity: float = 1.0
    entry_price: float = 0.0
    parameters: Dict[str, Any] = Field(default_factory=dict)
    result: Optional[PositionResult] = None


class Portfolio(BaseModel):
    """Full portfolio model stored as JSON in GCS."""
    id: str
    name: str = "Untitled Portfolio"
    owner: str = ""  # username; empty = anonymous / local
    created_at: str = Field(default_factory=lambda: datetime.utcnow().isoformat())
    updated_at: str = Field(default_factory=lambda: datetime.utcnow().isoformat())
    positions: List[Position] = Field(default_factory=list)


class PortfolioSummary(BaseModel):
    """Lightweight portfolio info for list endpoint."""
    id: str
    name: str
    created_at: str
    updated_at: str
    n_positions: int
    total_value: float


# ---------------------------------------------------------------------------
# Risk / VaR models
# ---------------------------------------------------------------------------

class StressBump(BaseModel):
    """A single scenario bump."""
    name: str = "Custom"
    spot_shift: float = 0.0       # relative, e.g. -0.10 = -10%
    vol_shift: float = 0.0        # absolute, e.g. 0.05 = +5 vol pts
    rate_shift: float = 0.0       # absolute, e.g. 0.01 = +100 bps


class StressResult(BaseModel):
    """Result for one stress scenario."""
    name: str
    portfolio_pnl: float
    position_pnls: Dict[str, float]  # position_id -> pnl


class VaRRequest(BaseModel):
    portfolio_id: str
    confidence: float = Field(0.95, ge=0.5, le=0.999)
    horizon_days: int = Field(1, ge=1, le=30)
    method: str = "parametric"  # "parametric" | "historical"


class VaRResult(BaseModel):
    confidence: float
    horizon_days: int
    var: float
    expected_shortfall: float
    method: str


class PortfolioRiskSummary(BaseModel):
    """Aggregated risk metrics for a portfolio."""
    total_npv: float
    total_pnl: float
    total_delta: float
    total_gamma: float
    total_vega: float
    total_theta: float
    total_rho: float
    positions_priced: int
    positions_total: int


class BatchPriceRequest(BaseModel):
    portfolio_id: str


class BatchPriceResponse(BaseModel):
    portfolio: Portfolio
    risk_summary: PortfolioRiskSummary
