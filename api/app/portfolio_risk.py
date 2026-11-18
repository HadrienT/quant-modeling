"""Stress tests and value at risk of a portfolio's trade ledger (issue #292).

Both read the same positions and the same daily marks as the snapshot
(portfolio_valuation.py): the holdings the transactions leave on the date,
each instrument fully revalued by its desk model.

**Stress tests.** Each position is revalued under a moved market of the
valuation date and compared with its mark: an equity spot shock (every
underlying's close times a factor; the smile is held in strike, "sticky
strike"), a parallel shift of the volatility the models run at, a parallel
shift of every zero rate (government curves for the derivatives, swap curves
for the rate and FX trades), and a crash that moves spot and volatility
together. A product whose volatility is a calibrated surface (a path-dependent
product under stochastic-local or local volatility) cannot have that surface
shifted here: in a volatility scenario it is not revalued and is listed as
such, never counted as unmoved. The stressed marks are never stored with the
official end-of-day marks.

**Value at risk.** Historical simulation on the marks: the holdings of the
valuation date, applied to each day of the window's change of every
instrument's value in the base currency. A listed instrument's change is
taken as a return, scaled to today's value (a share's move is proportional to
its price); a derivative's or a rate trade's as the change of its value per
unit, the contract fully revalued on each day with that day's market. The VaR
is the loss not exceeded on (confidence) of those days, the expected
shortfall the average loss beyond it, both over one day. The VaR's interval
is distribution-free (order statistics: the binomial law of the number of
scenarios below a quantile), the expected shortfall's a bootstrap of the
scenarios.
"""

from __future__ import annotations

import math
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import date
from typing import Callable, Dict, List, Optional, Tuple

import numpy as np
from scipy.stats import binom

from . import portfolio_rates
from . import portfolio_valuation as pv
from .portfolio_ledger import QTY_EPS
from .portfolio_marks import Mark
from .portfolio_schemas import (
    DerivativeSpec,
    ScriptSpec,
    EquitySpec,
    Instrument,
    Portfolio,
    RateFxSpec,
)
from .tasks import Cancelled

#: Models whose volatility is a calibrated surface: a parallel shift is not
#: implemented for them (see the module's docstring).
SURFACE_MODELS = ("Stochastic-local volatility", "Dupire local volatility")


@dataclass(frozen=True)
class Scenario:
    name: str
    description: str
    spot: float = 1.0  # factor on every equity close
    vol: float = 0.0  # absolute shift of the volatility
    rate: float = 0.0  # absolute shift of every zero rate


SCENARIOS: Tuple[Scenario, ...] = (
    Scenario(
        "Equities −20%",
        "Every equity underlying 20 % lower, smile held in strike",
        spot=0.8,
    ),
    Scenario(
        "Equities −10%",
        "Every equity underlying 10 % lower, smile held in strike",
        spot=0.9,
    ),
    Scenario(
        "Equities +10%",
        "Every equity underlying 10 % higher, smile held in strike",
        spot=1.1,
    ),
    Scenario(
        "Equities +20%",
        "Every equity underlying 20 % higher, smile held in strike",
        spot=1.2,
    ),
    Scenario(
        "Volatility +5 pts",
        "The volatility every model runs at, 5 points higher",
        vol=0.05,
    ),
    Scenario(
        "Volatility −5 pts",
        "The volatility every model runs at, 5 points lower",
        vol=-0.05,
    ),
    Scenario("Rates +100 bp", "Every zero rate of every curve 1 % higher", rate=0.01),
    Scenario("Rates −100 bp", "Every zero rate of every curve 1 % lower", rate=-0.01),
    Scenario(
        "Crash",
        "Equities 20 % lower and volatility 10 points higher",
        spot=0.8,
        vol=0.10,
    ),
)


class _Scaled:
    """A dated series whose value on `d` is multiplied by `factor`."""

    def __init__(self, series, d: date, factor: float):
        self._s, self._d, self._f = series, d, factor
        self.dates, self.values = series.dates, series.values

    def at(self, when: date):
        got = self._s.at(when)
        if got is None or when != self._d or self._f == 1.0:
            return got
        return got[0], got[1] * self._f

    def __bool__(self) -> bool:
        return bool(self._s)


class _Curve:
    """A discount curve series whose curve on `d` has every zero rate moved by
    `shift`."""

    def __init__(self, series, d: date, shift: float):
        self._s, self._d, self._shift = series, d, shift

    def at(self, when: date):
        got = self._s.at(when)
        if got is None or when != self._d or not self._shift:
            return got
        obs, (times, dfs) = got
        return obs, (
            times,
            [f * math.exp(-self._shift * t) for f, t in zip(dfs, times)],
        )


class _Shocked:
    """The market of the valuation date with a scenario applied, as the marks
    read it; everything else is the base market's. Its own memo of marks: a
    stressed mark must not be served for the official one."""

    def __init__(self, md: pv.MarketData, d: date, sc: Scenario):
        self._md, self._d, self._sc = md, d, sc
        self.vol_shift = sc.vol
        self.marks: Dict = {}

    def __getattr__(self, name):
        return getattr(self._md, name)

    def closes(self, ticker: str):
        return _Scaled(self._md.closes(ticker), self._d, self._sc.spot)

    def closes_from(self, ticker: str, since: date):
        return _Scaled(self._md.closes_from(ticker, since), self._d, self._sc.spot)

    def curve(self, ccy: str):
        return _Curve(self._md.curve(ccy), self._d, self._sc.rate)

    def zero_rate(self, ccy: str, d: date, t: float):
        return pv._zero_rate(self, ccy, d, t)

    def realised_vol(self, ticker: str, d: date):
        # The base proxy: the scenario's shift is applied where the vol is
        # used (portfolio_models._asset_vol, the catalog fallback), once.
        return self._md.realised_vol(ticker, d)


def _shocked_mark(inst: Instrument, d: date, md: pv.MarketData, sc: Scenario) -> Mark:
    spec = inst.spec
    if isinstance(spec, EquitySpec):
        base = pv.mark(inst, d, md)
        if base.value is None:
            return base
        return Mark(base.value * sc.spot, base.currency)
    if isinstance(spec, RateFxSpec):
        moved = md
        if sc.rate:
            for ccy in ("USD", "EUR"):
                moved = portfolio_rates._Moved(moved, d, ccy, curve=sc.rate)
        return portfolio_rates.mark(spec, d, moved) if sc.rate else pv.mark(inst, d, md)
    if isinstance(spec, ScriptSpec) and spec.curve == "sofr":
        if not sc.rate:
            return pv.mark(inst, d, md)
        return pv._mark_derivative(
            spec, d, portfolio_rates._Moved(md, d, "USD", curve=sc.rate), d
        )
    return pv._mark_derivative(spec, d, _Shocked(md, d, sc), d)


def _underlying(inst: Instrument) -> str:
    spec = inst.spec
    if isinstance(spec, EquitySpec):
        return spec.ticker
    if isinstance(spec, DerivativeSpec):
        return spec.underlying or spec.product
    if isinstance(spec, ScriptSpec):
        return spec.underlying or f"{spec.title or spec.product} ({spec.currency})"
    return f"{spec.product} ({spec.currency})"


@dataclass
class StressRow:
    instrument_id: str
    label: str
    underlying: str
    pnl: Optional[float]  # base currency; None when not revalued
    note: str = ""


@dataclass
class StressResult:
    scenario: Scenario
    pnl: float  # sum over the revalued positions
    rows: List[StressRow]
    by_underlying: Dict[str, float]
    not_revalued: List[str]


def stress(
    pf: Portfolio, as_of: date, md: Optional[pv.MarketData] = None
) -> Tuple[List[StressResult], List[str]]:
    pf = pv.with_contract_starts(pf)
    md = md or pv.MarketData(as_of)
    by_id = {i.id: i for i in pf.instruments}
    held = [
        (by_id[iid], h.quantity)
        for iid, h in pv._holdings_at(pf, as_of).items()
        if abs(h.quantity) > QTY_EPS
    ]
    pv.prefetch_marks(pf, [as_of], md)
    warnings: List[str] = []
    base: Dict[str, Optional[float]] = {}
    for inst, qty in held:
        base[inst.id] = pv._value_base(inst, qty, as_of, md, pf.base_currency)
        if base[inst.id] is None:
            warnings.append(
                f"{inst.label or inst.id} has no mark on {as_of}: left out of every scenario."
            )
    results = []
    for sc in SCENARIOS:
        rows: List[StressRow] = []
        not_revalued: List[str] = []
        for inst, qty in held:
            label = inst.label or inst.id
            if base[inst.id] is None:
                continue
            m = _shocked_mark(inst, as_of, md, sc)
            surface = bool(sc.vol) and (
                (
                    m.model is not None
                    and any(m.model.model.startswith(s) for s in SURFACE_MODELS)
                )
                or (
                    isinstance(inst.spec, RateFxSpec)
                    and inst.spec.product in ("swaption", "bermudan")
                )
            )
            fxr = md.fx(pv._ccy(inst, md), pf.base_currency).at(as_of)
            if surface:
                note = (
                    "its normal volatility (bp) is not in the units of this shift"
                    if isinstance(inst.spec, RateFxSpec)
                    else f"{m.model.model}: its volatility surface is not shifted here"
                )
                rows.append(StressRow(inst.id, label, _underlying(inst), None, note))
                not_revalued.append(label)
            elif m.value is None or fxr is None:
                rows.append(
                    StressRow(
                        inst.id,
                        label,
                        _underlying(inst),
                        None,
                        m.note or "no stressed mark",
                    )
                )
                not_revalued.append(label)
            else:
                pnl = qty * m.value * fxr[1] - base[inst.id]
                rows.append(StressRow(inst.id, label, _underlying(inst), pnl))
        by_und: Dict[str, float] = {}
        for r in rows:
            if r.pnl is not None:
                by_und[r.underlying] = by_und.get(r.underlying, 0.0) + r.pnl
        results.append(
            StressResult(
                sc,
                sum(r.pnl for r in rows if r.pnl is not None),
                rows,
                by_und,
                not_revalued,
            )
        )
    return results, warnings


# ── Value at risk ────────────────────────────────────────────────────────────

#: Business days of the window, and the fewest scenarios for a VaR at all.
VAR_WINDOW = 250
VAR_MIN_SCENARIOS = 60
CONFIDENCES = (0.99, 0.975)
BOOTSTRAP = 2000
BOOTSTRAP_SEED = 11


@dataclass
class Estimate:
    value: float
    lower: float
    upper: float
    confidence: float  # of the interval
    method: str


@dataclass
class VarLevel:
    confidence: float
    var: Estimate
    expected_shortfall: Estimate
    exceedances: int  # scenarios with a loss beyond the VaR


@dataclass
class VarResult:
    as_of: date
    window: Tuple[date, date]
    scenarios: List[float]  # P&L of today's holdings on each usable day
    dates: List[date]
    levels: List[VarLevel]
    instruments: int
    left_out: List[str] = field(default_factory=list)
    days_dropped: int = 0


def unit_values(
    pf: Portfolio,
    insts: List[Instrument],
    days: List[date],
    md: pv.MarketData,
    progress: Optional[Callable[[float], bool]] = None,
) -> np.ndarray:
    """days × instruments: one unit of each, in the base currency, NaN without
    a mark. Every derivative fully revalued with each day's market (the
    official marks, computed once and stored). `progress(fraction)` is called
    as the marks come; a true answer stops the run (tasks.Cancelled)."""
    jobs = [
        (i, d)
        for d in days
        for i in insts
        if isinstance(i.spec, (DerivativeSpec, RateFxSpec, ScriptSpec))
    ]
    for inst, _ in jobs:  # load the series once, as prefetch_marks does
        spec = inst.spec
        underlying = getattr(spec, "underlying", None)  # a rate trade has none
        if underlying:
            md.closes(underlying)
            md.currency(underlying)
            md.dividends(underlying)
        md.curve(spec.currency)
    done = {"n": 0}
    lock = threading.Lock()

    def one(job) -> None:
        if progress is not None and progress(done["n"] / max(len(jobs), 1)):
            raise Cancelled()
        pv.mark(job[0], job[1], md)
        with lock:
            done["n"] += 1

    if jobs:
        with ThreadPoolExecutor(min(pv.WORKERS, len(jobs))) as ex:
            list(ex.map(one, jobs))
    out = np.full((len(days), len(insts)), np.nan)
    for j, inst in enumerate(insts):
        for k, d in enumerate(days):
            v = pv._value_base(inst, 1.0, d, md, pf.base_currency)
            if v is not None:
                out[k, j] = v
    return out


def _quantile_interval(
    sorted_pnl: np.ndarray, p: float, level: float = 0.95
) -> Tuple[float, float]:
    """Distribution-free interval of the p-quantile: order statistics x_(l),
    x_(u) with P(l <= B < u) >= level, B ~ Binomial(n, p) the number of
    scenarios below the quantile."""
    n = len(sorted_pnl)
    lo = int(binom.ppf((1 - level) / 2, n, p))
    hi = int(binom.ppf(1 - (1 - level) / 2, n, p)) + 1
    lo, hi = max(lo, 1), min(hi, n)
    return float(sorted_pnl[lo - 1]), float(sorted_pnl[hi - 1])


def _ordinal(k: int) -> str:
    if 10 <= k % 100 <= 20:
        return f"{k}th"
    return f"{k}{ {1: 'st', 2: 'nd', 3: 'rd'}.get(k % 10, 'th') }"


def _levels(pnl: np.ndarray) -> List[VarLevel]:
    ordered = np.sort(pnl)
    n = len(ordered)
    rng = np.random.default_rng(BOOTSTRAP_SEED)
    out = []
    for c in CONFIDENCES:
        p = 1.0 - c
        k = max(int(math.ceil(p * n)), 1)  # the k-th worst scenario
        q = ordered[k - 1]
        q_lo, q_hi = _quantile_interval(ordered, p)
        tail = ordered[:k]
        es = -float(tail.mean())
        boots = []
        for _ in range(BOOTSTRAP):
            sample = np.sort(rng.choice(pnl, size=n, replace=True))
            boots.append(-float(sample[:k].mean()))
        es_lo, es_hi = np.percentile(boots, [2.5, 97.5])
        out.append(
            VarLevel(
                confidence=c,
                var=Estimate(
                    -float(q),
                    -q_hi,
                    -q_lo,
                    0.95,
                    f"the {_ordinal(k)} worst of {n} days; interval from the order statistics (binomial law)",
                ),
                expected_shortfall=Estimate(
                    es,
                    float(es_lo),
                    float(es_hi),
                    0.95,
                    f"mean loss of the {k} worst of {n} days; interval from {BOOTSTRAP} bootstrap resamples",
                ),
                exceedances=int((pnl < q).sum()),
            )
        )
    return out


def value_at_risk(
    pf: Portfolio,
    as_of: date,
    window: int = VAR_WINDOW,
    progress: Optional[Callable[[float], bool]] = None,
) -> VarResult:
    pf = pv.with_contract_starts(pf)
    by_id = {i.id: i for i in pf.instruments}
    holdings = {
        iid: h.quantity
        for iid, h in pv._holdings_at(pf, as_of).items()
        if abs(h.quantity) > QTY_EPS
    }
    if not holdings:
        raise ValueError(f"the portfolio holds nothing on {as_of}")
    days = pv.business_days(
        np.busday_offset(as_of, -window, roll="backward").astype(date), as_of
    )
    md = pv.MarketData(days[0])
    insts = [by_id[i] for i in holdings]
    values = unit_values(pf, insts, days, md, progress)
    qty = np.array([holdings[i.id] for i in insts])
    today = values[-1]
    left_out = [i.label or i.id for i, v in zip(insts, today) if not np.isfinite(v)]
    keep = np.isfinite(today)
    listed = np.array([isinstance(i.spec, EquitySpec) for i in insts])
    # Change of each instrument's value per unit, as of today's holdings:
    # a listed one by its return times today's value, the others absolute.
    change = np.diff(values, axis=0)
    rel = change / values[:-1] * today
    unit = np.where(listed, rel, change)[:, keep]
    usable = np.all(np.isfinite(unit), axis=1)
    pnl = unit[usable] @ qty[keep]
    dates = [d for d, u in zip(days[1:], usable) if u]
    if len(pnl) < VAR_MIN_SCENARIOS:
        raise ValueError(
            f"only {len(pnl)} days of the window have a mark for every position "
            f"(at least {VAR_MIN_SCENARIOS} are needed)"
        )
    return VarResult(
        as_of=as_of,
        window=(days[1], days[-1]),
        scenarios=[float(x) for x in pnl],
        dates=dates,
        levels=_levels(pnl),
        instruments=int(keep.sum()),
        left_out=left_out,
        days_dropped=int((~usable).sum()),
    )


# ── Backtest of the VaR (WP 24, the risk-model tab) ──────────────────────────


@dataclass
class VarHistory:
    """The VaR announced each day for the next, and the day's hypothetical
    P&L (the holdings of the day before, held still)."""

    dates: List[date]
    pnl: List[float]
    var: List[float]  # positive, a loss
    confidence: float
    window: int
    skipped: int  # days with no holdings, or too few scenarios behind them
    values_fingerprint: str


def var_history(
    pf: Portfolio,
    end: date,
    days: int = 250,
    window: int = VAR_WINDOW,
    confidence: float = 0.99,
    progress: Optional[Callable[[float], bool]] = None,
) -> VarHistory:
    """For each of the last `days` business days t up to `end`: the VaR of
    the holdings of t−1 on the `window` days before t (the same historical
    simulation as value_at_risk, announced the evening before), and the
    hypothetical P&L of t: those holdings on t's change of every instrument.
    The model is judged on that P&L, not on the actual one, which the day's
    trades would move (Basel's backtesting of internal models)."""
    import hashlib

    pf = pv.with_contract_starts(pf)
    test = pv.business_days(
        np.busday_offset(end, -(days - 1), roll="backward").astype(date), end
    )
    first = np.busday_offset(test[0], -(window + 1), roll="backward").astype(date)
    calendar = pv.business_days(first, end)
    held_ids = sorted(
        {
            iid
            for d in [pv.previous_business_day(test[0])] + test
            for iid, h in pv._holdings_at(pf, d).items()
            if abs(h.quantity) > QTY_EPS
        }
    )
    if not held_ids:
        raise ValueError("the portfolio held nothing over the period")
    by_id = {i.id: i for i in pf.instruments}
    insts = [by_id[i] for i in held_ids]
    md = pv.MarketData(calendar[0])
    values = unit_values(pf, insts, calendar, md, progress)
    listed = np.array([isinstance(i.spec, EquitySpec) for i in insts])
    change = np.diff(values, axis=0)  # row k: calendar[k+1] against calendar[k]
    returns = change / values[:-1]
    p = 1.0 - confidence
    pos = {d: k for k, d in enumerate(calendar)}
    out_d, out_pnl, out_var, skipped = [], [], [], 0
    for t in test:
        k = pos.get(t)
        if k is None or k < window + 1:
            skipped += 1
            continue
        prev = calendar[k - 1]
        hold = pv._holdings_at(pf, prev)
        q = np.array([hold[i].quantity if i in hold else 0.0 for i in held_ids])
        live = np.abs(q) > QTY_EPS
        if not live.any() or not np.all(np.isfinite(values[k - 1, live])):
            skipped += 1
            continue
        # Scenarios: the window's changes before t, scaled as value_at_risk does.
        rows = slice(k - 1 - window, k - 1)
        unit = np.where(listed, returns[rows] * values[k - 1], change[rows])[:, live]
        usable = np.all(np.isfinite(unit), axis=1)
        today = change[k - 1, live]
        if usable.sum() < VAR_MIN_SCENARIOS or not np.all(np.isfinite(today)):
            skipped += 1
            continue
        scen = np.sort(unit[usable] @ q[live])
        n = len(scen)
        out_var.append(-float(scen[max(int(math.ceil(p * n)), 1) - 1]))
        out_pnl.append(float(today @ q[live]))
        out_d.append(t)
    finite = np.where(np.isfinite(values), values, 0.0)
    return VarHistory(
        out_d,
        out_pnl,
        out_var,
        confidence,
        window,
        skipped,
        hashlib.sha256(np.ascontiguousarray(finite).tobytes()).hexdigest(),
    )
