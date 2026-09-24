"""Portfolio ledger — the transactions are the source of truth; positions are
derived from them.

A trade buys (quantity > 0) or sells (quantity < 0) an instrument at a unit
price, in the instrument's currency, with fees. Selling more than is held
opens a short position; buying it back closes it. Nothing else changes a
position: there is no "edit the quantity", only another trade — which is what
makes the history of the portfolio reconstructible at any date.

Cost method: weighted average cost (prix moyen pondéré, the French tax
convention for shares). A trade that ADDS to a position moves the average
cost; a trade that REDUCES it realises (price − average cost) × quantity
closed and leaves the average unchanged; a trade that goes through zero
closes the old position and opens the new one at the trade price. Fees are
charged to realised P&L when paid.

Everything here is pure (no database, no pricing): `portfolio_valuation.py`
marks the positions to market.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from typing import Dict, Iterable, List, Optional

from .portfolio_schemas import (
    DerivativeSpec,
    EquitySpec,
    Instrument,
    Portfolio,
    Position,
    Trade,
)

#: Below this, a quantity is zero (floating residue of partial sales).
QTY_EPS = 1e-9


@dataclass
class Holding:
    """One instrument's position, as the trades up to some date leave it."""

    instrument_id: str
    quantity: float = 0.0
    average_cost: float = 0.0  # instrument currency, per unit; 0 when flat
    realised: float = 0.0  # instrument currency, fees included
    fees: float = 0.0
    trades: int = 0
    opened: Optional[date] = None

    def apply(self, trade: Trade) -> None:
        q, p = trade.quantity, trade.price
        self.trades += 1
        self.fees += trade.fees
        self.realised -= trade.fees
        held = self.quantity
        if abs(held) < QTY_EPS or math.copysign(1, q) == math.copysign(1, held):
            # Opening or adding: the average cost moves.
            new = held + q
            self.average_cost = (self.average_cost * abs(held) + p * abs(q)) / abs(new)
            self.quantity = new
            if abs(held) < QTY_EPS:
                self.opened = trade.trade_date
            return
        # Reducing (or going through zero): realise on what is closed.
        closed = min(abs(q), abs(held))
        self.realised += (p - self.average_cost) * closed * math.copysign(1, held)
        remaining = abs(q) - closed
        self.quantity = held + math.copysign(closed, q)
        if abs(self.quantity) < QTY_EPS:
            self.quantity = 0.0
            self.average_cost = 0.0
        if remaining > QTY_EPS:
            # Through zero: the rest opens a position the other way, at p.
            self.quantity = math.copysign(remaining, q)
            self.average_cost = p
            self.opened = trade.trade_date


def sorted_trades(trades: Iterable[Trade]) -> List[Trade]:
    """Chronological: by trade date, then by entry order (created_at, id)."""
    return sorted(trades, key=lambda t: (t.trade_date, t.created_at, t.id))


def holdings(portfolio: Portfolio, as_of: Optional[date] = None) -> Dict[str, Holding]:
    """Every instrument's holding after the trades dated on or before `as_of`
    (all trades when None). Instruments fully closed are kept: their
    realised P&L is still part of the portfolio's."""
    out: Dict[str, Holding] = {}
    for t in sorted_trades(portfolio.transactions):
        if as_of is not None and t.trade_date > as_of:
            continue
        out.setdefault(t.instrument_id, Holding(t.instrument_id)).apply(t)
    return out


def validate(portfolio: Portfolio) -> None:
    """Every trade points to a known instrument (raises ValueError)."""
    known = {i.id for i in portfolio.instruments}
    unknown = sorted({t.instrument_id for t in portfolio.transactions} - known)
    if unknown:
        raise ValueError(f"trades reference unknown instruments: {unknown}")


# ── Migration of the position-based portfolios (before the ledger) ───────────

#: Legacy product_type -> the pricing registry's product id (valuation.PRODUCTS).
#: Both spellings existed: the API's enum and the front's catalog keys.
_LEGACY_PRODUCTS = {
    "european-call": "vanilla",
    "european-put": "vanilla",
    "american-call": "american_vanilla",
    "american-put": "american_vanilla",
    "vanilla": "vanilla",
    "american": "american_vanilla",
    "quanto": "quanto",
    "asian": "asian",
    "barrier": "barrier",
    "digital": "digital",
    "lookback": "lookback",
    "basket": "basket",
    "rainbow": "rainbow",
    "future": "future",
    "zero-coupon-bond": "zero_coupon_bond",
    "fixed-rate-bond": "fixed_rate_bond",
    "autocall": "autocall",
    "mountain": "mountain",
    "variance-swap": "variance_swap",
    "volatility-swap": "volatility_swap",
    "dispersion-swap": "dispersion_swap",
    "fx-forward": "fx_forward",
    "fx-option": "fx_option",
    "commodity-forward": "commodity_forward",
    "commodity-option": "commodity_option",
}
#: The API's own enum values stored decimals; the front's catalog keys stored
#: the FORM values, where these fields are in percent.
_CATALOG_KEYS = {"vanilla", "american", "quanto", "asian", "barrier", "digital",
                 "lookback", "basket", "rainbow", "future", "zero-coupon-bond",
                 "fixed-rate-bond"}  # fmt: skip
_PERCENT_FIELDS = {"rate", "dividend", "vol", "rate_domestic", "rate_foreign",
                   "fx_vol", "coupon_rate"}  # fmt: skip
_PERCENT_LISTS = {"vols", "dividends"}


def _legacy_params(position: Position) -> dict:
    params = dict(position.parameters)
    if position.product_type in ("european-call", "american-call"):
        params.setdefault("is_call", True)
    if position.product_type in ("european-put", "american-put"):
        params.setdefault("is_call", False)
    if position.product_type in _CATALOG_KEYS:
        for k in _PERCENT_FIELDS & params.keys():
            params[k] = float(params[k]) / 100.0
        for k in _PERCENT_LISTS & params.keys():
            params[k] = [float(v) / 100.0 for v in params[k]]
    return params


def migrate(portfolio: Portfolio) -> Portfolio:
    """A portfolio of positions becomes a ledger: each position is an
    instrument plus one trade (quantity signed by its direction, at its
    entry price, dated the portfolio's creation). Idempotent; a portfolio
    that already has a ledger is returned unchanged."""
    if portfolio.version >= 2:
        return portfolio
    try:
        opened = datetime.fromisoformat(portfolio.created_at).date()
    except ValueError:
        opened = date.today()
    instruments: List[Instrument] = list(portfolio.instruments)
    trades: List[Trade] = list(portfolio.transactions)
    unmigrated: List[Position] = []
    for pos in portfolio.positions:
        product = _LEGACY_PRODUCTS.get(str(pos.product_type))
        if product is None:
            # Never dropped silently: kept as is, and the valuation says so.
            unmigrated.append(pos)
            continue
        params = _legacy_params(pos)
        maturity = float(params.get("maturity", 0) or 0)
        instruments.append(
            Instrument(
                id=pos.id,
                label=f"{pos.label} (migrated)",
                spec=DerivativeSpec(
                    product=product,
                    params=params,
                    expiry=opened + timedelta(days=round(maturity * 365.25)),
                ),
            )
        )
        sign = -1.0 if pos.direction == "short" else 1.0
        trades.append(
            Trade(
                id=f"{pos.id}-open",
                instrument_id=pos.id,
                trade_date=opened,
                quantity=sign * float(pos.quantity or 1.0),
                price=float(pos.entry_price or 0.0),
                note="Migrated from the position-based portfolio",
            )
        )
    return portfolio.model_copy(
        update={
            "instruments": instruments,
            "transactions": trades,
            "positions": unmigrated,
            "version": 2,
        }
    )


def equity_instrument_id(ticker: str) -> str:
    """One instrument per listed ticker: every trade on it aggregates."""
    return f"EQ:{ticker.strip().upper()}"


def is_equity(instrument: Instrument) -> bool:
    return isinstance(instrument.spec, EquitySpec)


__all__ = [
    "Holding",
    "QTY_EPS",
    "equity_instrument_id",
    "holdings",
    "is_equity",
    "migrate",
    "sorted_trades",
    "validate",
]
