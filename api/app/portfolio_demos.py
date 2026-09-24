"""Demo portfolios — read-only ledgers anyone can open on the Portfolio page.

A demo states WHAT was traded and WHEN, never at what price: each trade is
priced when the demo is built, from the same market data the valuation uses
(portfolio_valuation.py). A stock trades at its close on the trade date (a
date without a stored close makes the demo unavailable, never priced at an
older close); a
derivative at its model value that day (the mark, with that day's spot,
curve, dividend and realised-vol proxy). So a demo's P&L is the one those
trades would really have made, not an illustration.

An option's strike is given as a fraction of the underlying's close on the
first trade date (1.10 = 10 % out of the money for a call), then rounded to
the listed-strike grid of its price level.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import date, datetime, time, timezone
from functools import lru_cache
from typing import Dict, List, Optional, Tuple

from . import portfolio_valuation as pv
from .portfolio_ledger import equity_instrument_id, validate
from .portfolio_schemas import DerivativeSpec, EquitySpec, Instrument, Portfolio, Trade

#: Trade-time values of the market fields; every valuation replaces them.
_FALLBACK = {"rate": 0.03, "dividend": 0.0, "vol": 0.2, "engine": "analytic"}


@dataclass(frozen=True)
class Option:
    """A European option on a listed underlying, strike as moneyness."""

    key: str
    underlying: str
    is_call: bool
    moneyness: float
    expiry: date
    currency: str


@dataclass(frozen=True)
class Buy:
    """quantity > 0 buys, < 0 sells; `what` is a ticker or an Option key."""

    on: date
    what: str
    quantity: float
    fees: float = 0.0
    note: str = ""


@dataclass(frozen=True)
class Demo:
    id: str
    name: str
    description: str
    base_currency: str
    trades: Tuple[Buy, ...]
    options: Tuple[Option, ...] = field(default_factory=tuple)


DEMOS: Tuple[Demo, ...] = (
    Demo(
        id="demo-euro-blue-chips",
        name="Euro blue chips",
        description=(
            "Five large euro-area names bought in January, SAP trimmed in May "
            "(realised P&L at the average cost), BNP added in June."
        ),
        base_currency="EUR",
        trades=(
            Buy(date(2026, 1, 5), "MC.PA", 20, 5.0, "Initial allocation"),
            Buy(date(2026, 1, 5), "TTE.PA", 60, 5.0, "Initial allocation"),
            Buy(date(2026, 1, 5), "AIR.PA", 25, 5.0, "Initial allocation"),
            Buy(date(2026, 1, 5), "SAP.DE", 30, 5.0, "Initial allocation"),
            Buy(date(2026, 1, 5), "SIE.DE", 25, 5.0, "Initial allocation"),
            Buy(date(2026, 5, 4), "SAP.DE", -10, 5.0, "Trim a third"),
            Buy(date(2026, 6, 1), "BNP.PA", 80, 5.0, "Add a bank"),
        ),
    ),
    Demo(
        id="demo-us-long-short",
        name="US long/short",
        description=(
            "Long three large-cap tech names against a short in Exxon (sold "
            "without holding it), half of the short bought back in July."
        ),
        base_currency="USD",
        trades=(
            Buy(date(2026, 2, 2), "NVDA", 40, 1.0, "Long leg"),
            Buy(date(2026, 2, 2), "MSFT", 25, 1.0, "Long leg"),
            Buy(date(2026, 2, 2), "AAPL", 40, 1.0, "Long leg"),
            Buy(date(2026, 2, 2), "XOM", -60, 1.0, "Short leg: sold, not held"),
            Buy(date(2026, 7, 9), "XOM", 30, 1.0, "Cover half the short"),
        ),
    ),
    Demo(
        id="demo-options-overlay",
        name="Options overlay",
        description=(
            "A covered call (100 AAPL, 100 calls sold 10 % out of the money) and "
            "a protective put (100 SPY, 100 puts bought 10 % out of the money)."
        ),
        base_currency="USD",
        options=(
            Option("aapl-call", "AAPL", True, 1.10, date(2026, 12, 18), "USD"),
            Option("spy-put", "SPY", False, 0.90, date(2027, 3, 19), "USD"),
        ),
        trades=(
            Buy(date(2026, 3, 2), "AAPL", 100, 1.0, "Covered call: the stock"),
            Buy(date(2026, 3, 2), "aapl-call", -100, 1.0, "Covered call: calls sold"),
            Buy(date(2026, 3, 2), "SPY", 100, 1.0, "Protective put: the ETF"),
            Buy(date(2026, 3, 2), "spy-put", 100, 1.0, "Protective put: puts bought"),
        ),
    ),
    Demo(
        id="demo-global-multi-currency",
        name="Global multi-currency",
        description=(
            "Stocks quoted in USD, GBP, JPY and EUR, reported in euros: the P&L "
            "includes the currency moves since each purchase (ECB rates)."
        ),
        base_currency="EUR",
        trades=(
            Buy(date(2026, 1, 5), "AAPL", 30, 1.0, "USD"),
            Buy(date(2026, 1, 5), "AZN.L", 80, 5.0, "GBP"),
            Buy(date(2026, 1, 5), "7203.T", 400, 500.0, "JPY"),
            Buy(date(2026, 1, 5), "SAP.DE", 20, 5.0, "EUR"),
        ),
    ),
)


def _strike_step(level: float) -> float:
    """Listed strikes get coarser with the price: 0.5 around 40, 5 around
    250, 50 around 8000 — half a unit of the level's second digit."""
    return 5 * 10 ** (math.floor(math.log10(level)) - 2)


def _option_instrument(opt: Option, first: date, md: pv.MarketData) -> Instrument:
    got = md.closes(opt.underlying).at(first)
    if got is None:
        raise LookupError(f"no close for {opt.underlying} on {first}")
    spot = float(got[1])
    step = _strike_step(spot)
    strike = round(spot * opt.moneyness / step) * step
    kind = "call" if opt.is_call else "put"
    return Instrument(
        id=opt.key,
        label=f"{opt.underlying} {strike:g} {kind} {opt.expiry:%b-%y}",
        spec=DerivativeSpec(
            product="vanilla",
            underlying=opt.underlying,
            expiry=opt.expiry,
            currency=opt.currency,
            params={
                **_FALLBACK,
                "spot": spot,
                "strike": strike,
                "maturity": (opt.expiry - first).days / pv.YEAR_DAYS,
                "is_call": opt.is_call,
            },
        ),
    )


def build(demo: Demo, md: pv.MarketData) -> Portfolio:
    """The demo as a ledger, every trade priced from the market data."""
    first = min(t.on for t in demo.trades)
    instruments: Dict[str, Instrument] = {
        o.key: _option_instrument(o, first, md) for o in demo.options
    }
    trades: List[Trade] = []
    for n, t in enumerate(demo.trades):
        if t.what not in instruments:
            iid = equity_instrument_id(t.what)
            instruments[t.what] = Instrument(
                id=iid, label=t.what, spec=EquitySpec(ticker=t.what)
            )
        inst = instruments[t.what]
        m = pv.mark(inst, t.on, md)
        if m.value is None:
            raise LookupError(f"{demo.id}: no price for {t.what} on {t.on}: {m.note}")
        # A trade is priced on its own day, never at an older close.
        observed = next(
            (
                i.as_of
                for i in m.inputs
                if i.name in ("close", "spot") or i.name.startswith("spot ")
            ),
            None,
        )
        if observed != t.on:
            raise LookupError(f"{demo.id}: no close for {t.what} on {t.on}")
        trades.append(
            Trade(
                id=f"{demo.id}-{n + 1}",
                instrument_id=inst.id,
                trade_date=t.on,
                quantity=t.quantity,
                price=round(m.value, 4),
                fees=t.fees,
                note=t.note,
                created_at=datetime.combine(
                    t.on, time(17, 30), timezone.utc
                ).isoformat(),
            )
        )
    stamp = datetime.combine(first, time(9), timezone.utc).isoformat()
    pf = Portfolio(
        id=demo.id,
        name=demo.name,
        owner="demo",
        version=2,
        base_currency=demo.base_currency,
        created_at=stamp,
        updated_at=stamp,
        instruments=list(instruments.values()),
        transactions=trades,
    )
    validate(pf)
    return pf


@lru_cache(maxsize=2)
def _built(day: str) -> Tuple[Tuple[Demo, Optional[Portfolio], Optional[str]], ...]:
    """All demos, built once a day. A demo whose data is missing is reported
    with its reason rather than failing the others."""
    md = pv.MarketData(min(t.on for d in DEMOS for t in d.trades))
    out = []
    for demo in DEMOS:
        try:
            out.append((demo, build(demo, md), None))
        except LookupError as exc:
            out.append((demo, None, str(exc)))
    return tuple(out)


def demos(
    today: Optional[date] = None,
) -> Tuple[Tuple[Demo, Optional[Portfolio], Optional[str]], ...]:
    return _built((today or date.today()).isoformat())


__all__ = ["DEMOS", "Demo", "Buy", "Option", "build", "demos"]
