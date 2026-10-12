"""Portfolio schemas for the quant desk portfolio manager."""

from datetime import date, datetime
from enum import Enum
from typing import Any, Dict, List, Literal, Optional, Union

from pydantic import BaseModel, Field, field_validator, model_validator

from .xva_schemas import CsaInput, Rating, Sector

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
    """A position of the portfolios before the ledger (version 1) — read only to
    be migrated (portfolio_ledger.migrate). product_type was spelled two ways
    (this enum, or the front's catalog keys), hence a plain string."""

    id: str
    label: str = "Untitled"
    product_type: str
    category: str = ""
    direction: str = "long"  # "long" or "short"
    quantity: float = 1.0
    entry_price: float = 0.0
    parameters: Dict[str, Any] = Field(default_factory=dict)
    result: Optional[PositionResult] = None


Currency = Literal["USD", "EUR", "GBP", "JPY", "CHF"]


class EquitySpec(BaseModel):
    """A listed share or index, marked at its stored close (any market of the
    Market page). Its currency is the listing's (prices.equity_universe)."""

    kind: Literal["equity"] = "equity"
    ticker: str


class DerivativeSpec(BaseModel):
    """A derivative priced by the pricing registry (valuation.PRODUCTS).

    `params` is the pricing request at trade time (decimals, as the API takes
    them). At each valuation date its market fields are replaced by that
    day's market (portfolio_valuation.py): spot from `underlying`'s close, the
    time left to `expiry`, the rate from the currency's curve, the vol from
    the underlying's history — whatever the stored market allows."""

    kind: Literal["derivative"] = "derivative"
    product: str
    params: Dict[str, Any]
    underlying: Optional[str] = None
    expiry: date
    currency: Currency = "USD"


def add_years(d: date, years: int) -> date:
    """The same day `years` later (28 February for a 29 February)."""
    try:
        return d.replace(year=d.year + years)
    except ValueError:
        return d.replace(year=d.year + years, day=28)


def whole_years(start: date, end: date) -> int:
    """How many years from `start` to `end` when it is a whole number, 0
    otherwise."""
    n = end.year - start.year
    return n if n > 0 and add_years(start, n) == end else 0


class RateFxSpec(BaseModel):
    """An over-the-counter rate or FX trade, marked each day on the swap
    curves of the store (portfolio_rates.py) and simulated by the exposure
    engine (xVA). One contract is the whole notional: a trade's quantity is
    +1 for the side described here, -1 for the other.

    The conventions are the exposure engine's: both legs of a swap pay once a
    year and it lasts a whole number of years; an FX trade is in euros
    against dollars and is valued in dollars.

    | product | start | maturity | payer | fixed_rate |
    |---|---|---|---|---|
    | swap | first day | last payment | pays the fixed rate | the fixed rate |
    | swaption | expiry | end of the swap | on a payer swap | the strike |
    | fx_forward | trade date | delivery | buys the euros | USD per EUR |
    | cross_currency_swap | first day | final exchange | receives the euros | the USD coupon |
    """

    kind: Literal["rate_fx"] = "rate_fx"
    product: Literal["swap", "swaption", "fx_forward", "cross_currency_swap"]
    currency: Literal["USD", "EUR"] = Field(
        description="The currency of the notional: EUR for an FX trade"
    )
    notional: float = Field(gt=0, le=1e12)
    payer: bool = True
    start: date
    maturity: date
    fixed_rate: float = Field(ge=-0.05, le=10.0)
    foreign_rate: Optional[float] = Field(
        default=None,
        ge=-0.05,
        le=0.5,
        description="The EUR coupon of a cross-currency swap",
    )
    domestic_notional: Optional[float] = Field(
        default=None,
        gt=0,
        le=1e13,
        description="The USD notional of a cross-currency swap: the EUR one at "
        "the exchange rate of its first day",
    )

    @model_validator(mode="after")
    def _coherent(self) -> "RateFxSpec":
        if self.maturity <= self.start:
            raise ValueError("the maturity must be after the start")
        fx = self.product in ("fx_forward", "cross_currency_swap")
        if fx and self.currency != "EUR":
            raise ValueError("an FX trade is in euros against dollars: currency EUR")
        if self.product != "fx_forward":
            n = whole_years(self.start, self.maturity)
            if not 1 <= n <= 30:
                raise ValueError(
                    "the maturity must be a whole number of years (1 to 30) after "
                    "the start: both legs pay once a year"
                )
        if self.product == "swaption" and (self.maturity - self.start).days > 30 * 366:
            raise ValueError("the swap of a swaption lasts at most 30 years")
        if self.product == "cross_currency_swap" and (
            self.foreign_rate is None or self.domestic_notional is None
        ):
            raise ValueError(
                "a cross-currency swap needs its EUR coupon and its USD notional"
            )
        return self

    @property
    def value_currency(self) -> str:
        """What a mark of the trade is in."""
        return (
            "USD"
            if self.currency == "EUR"
            and self.product
            in (
                "fx_forward",
                "cross_currency_swap",
            )
            else self.currency
        )

    @property
    def end(self) -> date:
        """The day after which the trade is no longer valued."""
        return self.start if self.product == "swaption" else self.maturity


class Instrument(BaseModel):
    id: str
    label: str = ""
    spec: Union[EquitySpec, DerivativeSpec, RateFxSpec] = Field(discriminator="kind")
    #: The netting set an over-the-counter derivative is traded under (one of
    #: the portfolio's counterparties'). None for a listed instrument, and for
    #: a derivative whose counterparty risk is not looked at. Not part of the
    #: spec: who the trade faces does not change its mark.
    netting_set_id: Optional[str] = None


class NettingSet(BaseModel):
    """The trades one master agreement closes out as a single net amount if
    the counterparty defaults, and the collateral agreement on that amount
    (None: uncollateralised)."""

    id: str
    name: str = Field(min_length=1, max_length=80)
    csa: Optional[CsaInput] = None


class Counterparty(BaseModel):
    """Who the over-the-counter trades of a portfolio face. The credit curve
    is the rating's (ICE BofA spreads, as on the credit and xVA pages); the
    sector is what the regulatory CVA capital weighs it by."""

    id: str
    name: str = Field(min_length=1, max_length=80)
    rating: Rating = "BBB"
    sector: Sector = "other"
    recovery: float = Field(default=0.4, ge=0.0, le=0.9)
    netting_sets: List[NettingSet] = Field(default_factory=list, max_length=10)


class Trade(BaseModel):
    """A buy (quantity > 0) or a sell (quantity < 0) at a unit price in the
    instrument's currency. Fees are a cost (>= 0)."""

    id: str
    instrument_id: str
    trade_date: date
    quantity: float
    price: float
    fees: float = Field(0.0, ge=0.0)
    note: str = ""
    created_at: str = Field(default_factory=lambda: datetime.utcnow().isoformat())

    @field_validator("quantity")
    @classmethod
    def _non_zero(cls, v: float) -> float:
        if v == 0 or v != v:
            raise ValueError("quantity must be non-zero (> 0 buys, < 0 sells)")
        return v


class Portfolio(BaseModel):
    """A portfolio, stored as JSON (storage.py). Version 2 is a ledger:
    `instruments` and `transactions`; positions are derived from them
    (portfolio_ledger.py). Version 1 held `positions` directly and is migrated
    on read."""

    id: str
    name: str = "Untitled Portfolio"
    owner: str = ""  # username; empty = anonymous / local
    created_at: str = Field(default_factory=lambda: datetime.utcnow().isoformat())
    updated_at: str = Field(default_factory=lambda: datetime.utcnow().isoformat())
    version: int = 1
    base_currency: Currency = "EUR"
    instruments: List[Instrument] = Field(default_factory=list)
    transactions: List[Trade] = Field(default_factory=list)
    counterparties: List[Counterparty] = Field(default_factory=list, max_length=50)
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
    spot_shift: float = 0.0  # relative, e.g. -0.10 = -10%
    vol_shift: float = 0.0  # absolute, e.g. 0.05 = +5 vol pts
    rate_shift: float = 0.0  # absolute, e.g. 0.01 = +100 bps


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
