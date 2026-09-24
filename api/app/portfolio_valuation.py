"""Portfolio valuation — marks every position to market at a date, the
portfolio's value in its base currency, and its daily P&L history.

Desk practice, within what the stored market data allows:

- **Listed instruments** (shares, indices) are marked at their close on or
  before the valuation date, in the listing currency.
- **Derivatives** are fully revalued each day with THAT day's market: the
  underlying's close for the spot, the time left to expiry (so the value
  decays), the zero rate at that horizon from the currency's government curve
  (rates.py), the underlying's dividend yield where stored — and for the
  volatility, the underlying's realised volatility over the last 63 business
  days, a PROXY (implied volatilities are not stored for these names). It is
  measured over TIME, not over a count of closes: squared log returns summed
  over the elapsed time (a return spanning missing days counts for all of
  them), so a gap in the stored closes neither shrinks nor stretches it. Each
  input carries its status — observed / stale / proxied / default, the
  vocabulary of the valuation record — so a mark says what it rests on. A
  field the market cannot supply (a multi-asset basket's correlations, a
  derivative with no underlying ticker) keeps its trade-time value, status
  `default`.
- **Base currency**: every mark is converted at the ECB reference rate of the
  valuation date (fx.py). Realised P&L is converted at the rate of the day it
  was realised.
- **Daily P&L** = V(d) − V(d−1) + cash flows of d, where V is the market value
  of the holdings at the end of d and a flow is −(quantity × price + fees)
  of each trade of d: what was paid out. Summed over a portfolio's life it is
  its total P&L (realised + unrealised, test_portfolio_valuation.py).
- **EOD marks are computed once**: a past date's mark of an instrument is
  stored (MarkStore) and reused, as a desk keeps its official end-of-day
  marks instead of recomputing the past each time.

An expired derivative is not valued past its expiry: the ledger needs the
settlement booked as a closing trade, and the position says so until then.
"""

from __future__ import annotations

import bisect
import hashlib
import json
import math
import os
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import date, timedelta
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd
import quantmodeling as qm

from . import db, fx, rates, valuation
from .portfolio_ledger import Holding, QTY_EPS, sorted_trades
from . import portfolio_models
from .portfolio_marks import Input, Mark, ModelInfo, ModelParam
from .portfolio_schemas import DerivativeSpec, EquitySpec, Instrument, Portfolio
from .storage import get_storage

YEAR_DAYS = 365.25
#: Close older than this (calendar days) is marked `stale`.
STALE_AFTER_DAYS = 5
#: Realised-vol proxy window, in business days (three months).
VOL_WINDOW = 63
#: Fewest returns in the window for a realised vol at all.
VOL_MIN_RETURNS = 20
#: Business days (Mon–Fri) in a year: the clock the realised vol runs on.
WEEKDAYS_PER_YEAR = YEAR_DAYS * 5 / 7
#: Asset/FX correlation window (calendar days) and fewest common returns.
CORR_DAYS = 365
CORR_MIN_RETURNS = 60
#: Market data read before a window, for the vol proxy and on-or-before lookups.
LOOKBACK_DAYS = 140


# ── Market data for a valuation run ──────────────────────────────────────────


class _OnOrBefore:
    """A dated series with on-or-before lookup."""

    def __init__(self, rows: List[Tuple[date, Any]]):
        rows = sorted(rows, key=lambda r: r[0])
        self.dates = [d for d, _ in rows]
        self.values = [v for _, v in rows]

    def at(self, d: date) -> Optional[Tuple[date, Any]]:
        i = bisect.bisect_right(self.dates, d) - 1
        return (self.dates[i], self.values[i]) if i >= 0 else None

    def __bool__(self) -> bool:
        return bool(self.dates)


class MarketData:
    """Everything one valuation run reads, fetched once per series."""

    def __init__(self, since: date):
        self.since = since - timedelta(days=LOOKBACK_DAYS)
        self._closes: Dict[str, _OnOrBefore] = {}
        self._fx: Dict[Tuple[str, str], _OnOrBefore] = {}
        self._curves: Dict[str, _OnOrBefore] = {}
        self._divs: Dict[str, _OnOrBefore] = {}
        self._ccy: Dict[str, str] = {}
        #: This run's marks, today's included (never stored): a mark asked
        #: twice — prefetched, then read — is computed once.
        self.marks: Dict[Tuple[str, date], Mark] = {}

    def closes(self, ticker: str) -> _OnOrBefore:
        if ticker not in self._closes:
            self._closes[ticker] = _OnOrBefore(db.price_history(ticker, self.since))
        return self._closes[ticker]

    def currency(self, ticker: str) -> str:
        if ticker not in self._ccy:
            self._ccy[ticker] = db.ticker_currency(ticker) or "USD"
        return self._ccy[ticker]

    def fx(self, ccy: str, base: str) -> _OnOrBefore:
        """base units per ccy unit."""
        key = (ccy, base)
        if key not in self._fx:
            if ccy == base:
                self._fx[key] = _OnOrBefore([(date(1900, 1, 1), 1.0)])
            else:
                s = fx.pair_history(ccy, base, self.since)
                self._fx[key] = _OnOrBefore(list(zip(s.index, s.values)))
        return self._fx[key]

    def curve(self, ccy: str) -> _OnOrBefore:
        """date → (times, dfs) of the currency's discount curve; empty when the
        currency has no derivable curve (GBP, CHF)."""
        if ccy not in self._curves:
            spec = rates.CATALOG.get(ccy)
            g = spec.government if spec else None
            rows: List[Tuple[date, Any]] = []
            if g is not None and not g.no_derivation:
                ids = [p.series_id for p in g.money_market + g.pillars]
                for d, vals in db.rates_curve_history(g.table, ids, self.since):
                    quoted = {k: v / 100.0 for k, v in vals.items()}
                    rows.append((d, rates.discount_curve(g, quoted)))
            self._curves[ccy] = _OnOrBefore(rows)
        return self._curves[ccy]

    def dividends(self, ticker: str) -> _OnOrBefore:
        if ticker not in self._divs:
            self._divs[ticker] = _OnOrBefore(
                db.dividend_yield_history(ticker, self.since)
            )
        return self._divs[ticker]

    def realised_vol(self, ticker: str, d: date) -> Optional[Tuple[date, float]]:
        return realised_vol_of(self.closes(ticker), d)

    def zero_rate(self, ccy: str, d: date, t: float) -> Optional[Tuple[date, float]]:
        return _zero_rate(self, ccy, d, t)

    def closes_from(self, ticker: str, since: date) -> _OnOrBefore:
        """The closes from `since` on — a contract's fixings may predate the
        window this run loaded."""
        if since < self.since:
            key = f"{ticker}@{since.isoformat()}"
            if key not in self._closes:
                self._closes[key] = _OnOrBefore(db.price_history(ticker, since))
            return self._closes[key]
        return self.closes(ticker)

    def fx_realised_vol(
        self, ccy: str, base: str, d: date
    ) -> Optional[Tuple[date, float]]:
        if ccy == base:
            return None
        try:
            return realised_vol_of(self.fx(ccy, base), d)
        except fx.FxUnavailable:
            return None

    def asset_fx_correlation(
        self, ticker: str, ccy: str, base: str, d: date
    ) -> Optional[Tuple[date, float]]:
        """Correlation of daily log returns of the asset (in its currency)
        and of the FX rate (base per ccy), over the year to d, on common
        dates. None below CORR_MIN_RETURNS common returns."""
        if ccy == base:
            return None
        try:
            fxs = self.fx(ccy, base)
        except fx.FxUnavailable:
            return None
        a = self.closes_from(ticker, d - timedelta(days=CORR_DAYS))
        lo = d - timedelta(days=CORR_DAYS)
        fx_by = dict(zip(fxs.dates, fxs.values))
        common = [
            (t, v, fx_by[t])
            for t, v in zip(a.dates, a.values)
            if lo <= t <= d and t in fx_by
        ]
        if len(common) - 1 < CORR_MIN_RETURNS:
            return None
        arr = np.log(np.asarray([[v, x] for _, v, x in common], dtype=float))
        r = np.diff(arr, axis=0)
        return common[-1][0], float(np.corrcoef(r[:, 0], r[:, 1])[0, 1])


def realised_vol_of(s: _OnOrBefore, d: date) -> Optional[Tuple[date, float]]:
    """Annualised realised vol over the VOL_WINDOW business days up to d:
    sqrt(sum r_i^2 / T), r_i the log returns between consecutive stored
    observations and T the business-day time they span, in years. The
    maximum-likelihood variance of a Brownian motion seen at irregular times
    (zero drift): missing closes lengthen a return's interval instead of
    being read as one-day moves, and the result does not depend on how much
    history was loaded before the window."""
    start = np.busday_offset(d, -VOL_WINDOW, roll="backward").astype(date)
    lo = bisect.bisect_left(s.dates, start)
    hi = bisect.bisect_right(s.dates, d)
    dates, values = s.dates[lo:hi], s.values[lo:hi]
    if len(dates) - 1 < VOL_MIN_RETURNS:
        return None
    r = np.diff(np.log(np.asarray(values, dtype=float)))
    elapsed = np.busday_count(dates[0], dates[-1]) / WEEKDAYS_PER_YEAR
    if elapsed <= 0:
        return None
    return dates[-1], float(math.sqrt(float(np.sum(r**2)) / elapsed))


# ── Marks ────────────────────────────────────────────────────────────────────


def _stale(d: date, observed: date) -> str:
    return "stale" if (d - observed).days > STALE_AFTER_DAYS else "observed"


def _mark_equity(spec: EquitySpec, d: date, md: MarketData) -> Mark:
    ccy = md.currency(spec.ticker)
    got = md.closes(spec.ticker).at(d)
    if got is None:
        return Mark(None, ccy, note=f"no close for {spec.ticker} on or before {d}")
    obs, close = got
    return Mark(
        float(close),
        ccy,
        [Input("close", _stale(d, obs), float(close), obs)],
        {"delta": 1.0},
    )


def _zero_rate(
    md: MarketData, ccy: str, d: date, t: float
) -> Optional[Tuple[date, float]]:
    got = md.curve(ccy).at(d)
    if got is None:
        return None
    obs, (times, dfs) = got
    # Outside the pillars the zero rate is held flat at the nearest pillar's
    # (a 2-week option on the ECB curve, which starts at 3M, uses the 3M rate).
    tq = min(max(t, times[0]), times[-1])
    [df] = qm.discount_factors(times, dfs, [tq])
    return obs, -math.log(df) / tq


def _mark_derivative(
    spec: DerivativeSpec, d: date, md: MarketData, today: Optional[date] = None
) -> Mark:
    ccy = spec.currency
    if d >= spec.expiry:
        return Mark(
            None,
            ccy,
            note=f"expired on {spec.expiry}: book its settlement as a closing trade",
        )
    product = valuation.PRODUCTS.get(spec.product)
    if product is None:
        return Mark(None, ccy, note=f"unknown product {spec.product!r}")
    params = dict(spec.params)
    inputs: List[Input] = []
    t = (spec.expiry - d).days / YEAR_DAYS
    if "maturity" in params:
        params["maturity"] = t
        inputs.append(Input("time to expiry", "observed", t, d))
    u = spec.underlying
    u_ccy = md.currency(u) if u else ccy
    if u and "spot" in params:
        got = md.closes(u).at(d)
        if got is not None:
            params["spot"] = float(got[1])
            inputs.append(Input(f"spot {u}", _stale(d, got[0]), params["spot"], got[0]))
        else:
            inputs.append(Input(f"spot {u}", "default", params["spot"]))
    elif "spot" in params:
        inputs.append(Input("spot", "default", params["spot"]))
    # Rates: the payment currency's curve; a quanto also needs the asset's.
    for field_name, rate_ccy in (
        ("rate", ccy),
        ("rate_domestic", ccy),
        ("rate_foreign", u_ccy),
    ):
        if field_name not in params:
            continue
        got = _zero_rate(md, rate_ccy, d, max(t, 1e-6))
        if got is not None:
            params[field_name] = got[1]
            inputs.append(
                Input(f"{field_name} ({rate_ccy} curve)", "observed", got[1], got[0])
            )
        else:
            inputs.append(
                Input(f"{field_name} ({rate_ccy})", "default", params[field_name])
            )
    if u and "dividend" in params:
        got = md.dividends(u).at(d)
        if got is not None:
            params["dividend"] = got[1]
            # A trailing yield moves slowly: stale only past 40 days.
            status = "observed" if (d - got[0]).days <= 40 else "stale"
            inputs.append(Input(f"dividend {u}", status, got[1], got[0]))
        else:
            inputs.append(Input("dividend", "default", params["dividend"]))
    try:
        desk = portfolio_models.price(
            spec, d, md, params, inputs, today or date.today()
        )
    except Exception as exc:  # noqa: BLE001 — one position must not sink the book
        return Mark(None, ccy, inputs, note=f"pricing failed: {exc}")
    if desk is not None:
        return desk
    # A product outside the desk policy: its catalog pricer, flat vol.
    if u and "vol" in params:
        got = md.realised_vol(u, d)
        if got is not None:
            params["vol"] = got[1]
            inputs.append(
                Input(f"vol {u} (63-day realised, proxy)", "proxied", got[1], got[0])
            )
        else:
            inputs.append(Input("vol", "default", params["vol"]))
    elif "vol" in params:
        inputs.append(Input("vol", "default", params["vol"]))
    try:
        req = product.request.model_validate(params)
        priced = valuation.price(spec.product, req)
    except Exception as exc:  # noqa: BLE001 — one position must not sink the book
        return Mark(None, ccy, inputs, note=f"pricing failed: {exc}")
    g = priced.response.greeks
    return Mark(
        float(priced.response.npv),
        ccy,
        inputs,
        {
            "delta": g.delta,
            "gamma": g.gamma,
            "vega": g.vega,
            "theta": g.theta,
            "rho": g.rho,
        },
        model=ModelInfo(
            "Black-Scholes, flat volatility",
            "catalog pricer",
            "A product outside the desk-model policy: its own pricer at the "
            "realised-volatility proxy.",
            [ModelParam(i.name, i.value, status=i.status) for i in inputs],
        ),
    )


class MarkStore:
    """Official end-of-day marks: a past date's mark of an instrument is
    computed once and kept (storage.py), keyed by the instrument's definition.
    Today's is not stored — the day's close may still come in."""

    PREFIX = "portfolio-marks"
    #: Bumped whenever the way a mark is computed changes, so that marks
    #: stored by an older method are never served as today's (v2: the desk-
    #: model policy of portfolio_models.py).
    METHOD = 2

    def __init__(self) -> None:
        self._mem: Dict[str, Dict[str, dict]] = {}
        # Marks are computed on parallel threads (prefetch_marks): one lock
        # keeps an instrument's file from being written by two at once.
        self._lock = threading.Lock()

    @staticmethod
    def key(instrument: Instrument) -> str:
        payload = json.dumps(
            {
                "method": MarkStore.METHOD,
                "spec": instrument.spec.model_dump(mode="json"),
            },
            sort_keys=True,
        )
        return hashlib.sha256(payload.encode()).hexdigest()[:32]

    def _load(self, k: str) -> Dict[str, dict]:
        if k not in self._mem:
            try:
                self._mem[k] = get_storage().read_json(f"{self.PREFIX}/{k}") or {}
            except Exception:  # noqa: BLE001 — a cache miss, not an error
                self._mem[k] = {}
        return self._mem[k]

    def get(self, instrument: Instrument, d: date) -> Optional[Mark]:
        with self._lock:
            raw = self._load(self.key(instrument)).get(d.isoformat())
        if raw is None:
            return None
        return Mark(
            raw["value"],
            raw["currency"],
            [
                Input(
                    i["name"],
                    i["status"],
                    i["value"],
                    date.fromisoformat(i["as_of"]) if i.get("as_of") else None,
                )
                for i in raw["inputs"]
            ],
            raw.get("greeks", {}),
            raw.get("note"),
            _model_from(raw.get("model")),
        )

    def put(self, instrument: Instrument, d: date, mark: Mark, today: date) -> None:
        if d >= today:
            return
        k = self.key(instrument)
        with self._lock:
            self._put(k, d, mark)

    def _put(self, k: str, d: date, mark: Mark) -> None:
        store = self._load(k)
        store[d.isoformat()] = {
            "value": mark.value,
            "currency": mark.currency,
            "inputs": [
                {**i.__dict__, "as_of": i.as_of.isoformat() if i.as_of else None}
                for i in mark.inputs
            ],
            "greeks": mark.greeks,
            "note": mark.note,
            "model": _model_to(mark.model),
        }
        try:
            get_storage().write_json(f"{self.PREFIX}/{k}", store)
        except Exception:  # noqa: BLE001 — losing the cache only costs time
            pass


def _model_to(m: Optional[ModelInfo]) -> Optional[dict]:
    if m is None:
        return None
    return {**m.__dict__, "params": [p.__dict__ for p in m.params]}


def _model_from(raw: Optional[dict]) -> Optional[ModelInfo]:
    if not raw:
        return None
    return ModelInfo(**{**raw, "params": [ModelParam(**p) for p in raw["params"]]})


_STORE = MarkStore()


def mark(
    instrument: Instrument, d: date, md: MarketData, today: Optional[date] = None
) -> Mark:
    today = today or date.today()
    if isinstance(instrument.spec, DerivativeSpec):
        memo = (MarkStore.key(instrument), d)
        if memo in md.marks:
            return md.marks[memo]
        m = _STORE.get(instrument, d)
        if m is None:
            m = _mark_derivative(instrument.spec, d, md, today)
            if m.value is not None:
                _STORE.put(instrument, d, m, today)
        md.marks[memo] = m
        return m
    return _mark_equity(instrument.spec, d, md)


#: Threads pricing derivative marks in parallel. One scripted pricing adds
#: no measurable memory (no RSS growth over repeated 20 000-path runs: the
#: engine keeps one path at a time), so the bound is CPU shared with the
#: other services on the machine, not RAM. QM_PORTFOLIO_WORKERS overrides.
WORKERS = max(1, int(os.getenv("QM_PORTFOLIO_WORKERS", "8")))


def prefetch_marks(pf: Portfolio, days: List[date], md: MarketData) -> None:
    """Price every derivative held on each of `days` on parallel threads
    (the pricers release the GIL), so the sequential P&L walk that follows
    only reads md.marks."""
    by_id = {i.id: i for i in pf.instruments}
    jobs = [
        (by_id[iid], d)
        for d in days
        for iid, h in _holdings_at(pf, d).items()
        if abs(h.quantity) > QTY_EPS and isinstance(by_id[iid].spec, DerivativeSpec)
    ]
    # Market series are read lazily and cached on md: load them once, here,
    # rather than race to load them from every thread.
    for inst, _ in jobs:
        spec = inst.spec
        if spec.underlying:
            md.closes(spec.underlying)
            md.currency(spec.underlying)
            md.dividends(spec.underlying)
        md.curve(spec.currency)
    if len(jobs) > 1:
        with ThreadPoolExecutor(min(WORKERS, len(jobs))) as ex:
            list(ex.map(lambda j: mark(j[0], j[1], md), jobs))


# ── Snapshot ─────────────────────────────────────────────────────────────────


@dataclass
class PositionView:
    instrument_id: str
    label: str
    kind: str
    currency: str
    quantity: float
    average_cost: float
    mark: Optional[float]
    market_value: Optional[float]  # instrument currency
    market_value_base: Optional[float]
    unrealised: Optional[float]  # instrument currency
    unrealised_base: Optional[float]
    realised_base: float
    fees_base: float
    day_pnl_base: Optional[float]
    fx_rate: Optional[float]
    inputs: List[Input]
    greeks: Dict[str, Optional[float]]
    note: Optional[str]
    model: Optional[ModelInfo] = None


@dataclass
class Snapshot:
    as_of: date
    base_currency: str
    positions: List[PositionView]
    market_value: float
    unrealised: float
    realised: float
    fees: float
    total_pnl: float
    day_pnl: Optional[float]
    warnings: List[str]


def _replay_base(pf: Portfolio, as_of: date, md: MarketData):
    """The ledger replayed twice: in each instrument's currency, and in the
    base currency with every trade converted at the ECB rate of its trade
    date. The base-currency replay gives the cost basis as it was actually
    paid in base currency — so unrealised P&L in base currency includes the
    FX move since purchase, and realised + unrealised equals the daily P&L
    summed (history), to the cent."""
    by_id = {i.id: i for i in pf.instruments}
    local: Dict[str, Holding] = {}
    base: Dict[str, Holding] = {}
    for t in sorted_trades(pf.transactions):
        if t.trade_date > as_of:
            continue
        ccy = _ccy(by_id[t.instrument_id], md)
        rate = md.fx(ccy, pf.base_currency).at(t.trade_date)
        r = rate[1] if rate else float("nan")
        local.setdefault(t.instrument_id, Holding(t.instrument_id)).apply(t)
        converted = t.model_copy(update={"price": t.price * r, "fees": t.fees * r})
        base.setdefault(t.instrument_id, Holding(t.instrument_id)).apply(converted)
    return local, base


def with_contract_starts(pf: Portfolio) -> Portfolio:
    """A path-dependent product observes from its contract's start: the
    `start_date` of its terms, or else the first trade on it (when it was
    struck). Written into the spec, so the stored marks are keyed by it."""
    first: Dict[str, date] = {}
    for t in pf.transactions:
        first[t.instrument_id] = min(
            first.get(t.instrument_id, t.trade_date), t.trade_date
        )
    out = []
    changed = False
    for i in pf.instruments:
        spec = i.spec
        if (
            isinstance(spec, DerivativeSpec)
            and spec.product in portfolio_models.SCRIPTED
            and "start_date" not in spec.params
            and i.id in first
        ):
            spec = spec.model_copy(
                update={
                    "params": {**spec.params, "start_date": first[i.id].isoformat()}
                }
            )
            i = i.model_copy(update={"spec": spec})
            changed = True
        out.append(i)
    return pf.model_copy(update={"instruments": out}) if changed else pf


def _ccy(instrument: Instrument, md: MarketData) -> str:
    spec = instrument.spec
    return (
        spec.currency if isinstance(spec, DerivativeSpec) else md.currency(spec.ticker)
    )


def previous_business_day(d: date) -> date:
    d -= timedelta(days=1)
    while d.weekday() >= 5:
        d -= timedelta(days=1)
    return d


def snapshot(pf: Portfolio, as_of: date, md: Optional[MarketData] = None) -> Snapshot:
    pf = with_contract_starts(pf)
    # From the first trade: its cost basis is converted at its trade-date FX.
    first = min((t.trade_date for t in pf.transactions), default=as_of)
    md = md or MarketData(min(first, previous_business_day(as_of)))
    prefetch_marks(pf, [previous_business_day(as_of), as_of], md)
    by_id = {i.id: i for i in pf.instruments}
    holding, base_holding = _replay_base(pf, as_of, md)
    prev = previous_business_day(as_of)
    prev_holdings = {k: v.quantity for k, v in _holdings_at(pf, prev).items()}
    warnings: List[str] = []
    views: List[PositionView] = []
    for iid, h in holding.items():
        inst = by_id[iid]
        ccy = _ccy(inst, md)
        fxr = md.fx(ccy, pf.base_currency).at(as_of)
        fx_rate = fxr[1] if fxr else None
        m = mark(inst, as_of, md) if abs(h.quantity) > QTY_EPS else Mark(0.0, ccy)
        mv = None if m.value is None else h.quantity * m.value
        unreal = None if m.value is None else (m.value - h.average_cost) * h.quantity
        conv = lambda x: None if x is None or fx_rate is None else x * fx_rate
        hb = base_holding[iid]
        # In base currency, against the cost basis paid in base currency.
        unreal_base = (
            None
            if mv is None or fx_rate is None
            else mv * fx_rate - hb.average_cost * h.quantity
        )
        day = _day_pnl(
            pf, inst, as_of, prev, h.quantity, prev_holdings.get(iid, 0.0), md
        )
        if m.note and abs(h.quantity) > QTY_EPS:
            warnings.append(f"{inst.label or iid}: {m.note}")
        views.append(
            PositionView(
                iid,
                inst.label or iid,
                inst.spec.kind,
                ccy,
                h.quantity,
                h.average_cost,
                m.value,
                mv,
                conv(mv),
                unreal,
                unreal_base,
                base_holding[iid].realised,
                base_holding[iid].fees,
                day,
                fx_rate,
                m.inputs,
                m.greeks,
                m.note,
                m.model,
            )
        )
    if pf.positions:
        warnings.append(
            f"{len(pf.positions)} position(s) of the old portfolio format could not be "
            "migrated (unknown product) and are not valued."
        )
    live = [v for v in views if v.market_value_base is not None]
    mv_total = sum(v.market_value_base for v in live)
    unreal_total = sum(v.unrealised_base or 0.0 for v in live)
    realised_total = sum(v.realised_base for v in views)
    days = [v.day_pnl_base for v in views]
    return Snapshot(
        as_of, pf.base_currency, views, mv_total, unreal_total, realised_total,
        sum(v.fees_base for v in views), realised_total + unreal_total,
        None if any(x is None for x in days) else sum(days), warnings,
    )  # fmt: skip


def _holdings_at(pf: Portfolio, d: date) -> Dict[str, Holding]:
    out: Dict[str, Holding] = {}
    for t in sorted_trades(pf.transactions):
        if t.trade_date <= d:
            out.setdefault(t.instrument_id, Holding(t.instrument_id)).apply(t)
    return out


def _value_base(
    inst: Instrument, qty: float, d: date, md: MarketData, base: str
) -> Optional[float]:
    if abs(qty) < QTY_EPS:
        return 0.0
    m = mark(inst, d, md)
    fxr = md.fx(_ccy(inst, md), base).at(d)
    if m.value is None or fxr is None:
        return None
    return qty * m.value * fxr[1]


def _flows_base(
    pf: Portfolio, inst_id: str, d: date, md: MarketData, ccy: str
) -> float:
    """What the trades of day d paid out, in base currency (negative = paid)."""
    fxr = md.fx(ccy, pf.base_currency).at(d)
    rate = fxr[1] if fxr else float("nan")
    return -sum(
        (t.quantity * t.price + t.fees) * rate
        for t in pf.transactions
        if t.instrument_id == inst_id and t.trade_date == d
    )


def _day_pnl(pf, inst, d, prev, qty_d, qty_prev, md) -> Optional[float]:
    v1 = _value_base(inst, qty_d, d, md, pf.base_currency)
    v0 = _value_base(inst, qty_prev, prev, md, pf.base_currency)
    if v1 is None or v0 is None:
        return None
    flows = sum(
        _flows_base(pf, inst.id, day, md, _ccy(inst, md))
        for day in _days_between(prev, d)
    )
    return v1 - v0 + flows


def _days_between(prev: date, d: date):
    """(prev, d]: the calendar days whose trades belong to d's P&L (a weekend
    trade counts on the next business day)."""
    day = prev + timedelta(days=1)
    while day <= d:
        yield day
        day += timedelta(days=1)


# ── History ──────────────────────────────────────────────────────────────────


@dataclass
class HistoryPoint:
    date: date
    market_value: Optional[float]
    daily_pnl: Optional[float]
    cumulative_pnl: Optional[float]


def business_days(start: date, end: date) -> List[date]:
    return [d.date() for d in pd.bdate_range(start, end)]


def history(
    pf: Portfolio, start: date, end: date
) -> Tuple[List[HistoryPoint], List[str]]:
    """Daily P&L of the portfolio in its base currency, from `start` (or its
    first trade, whichever is later) to `end`. The cumulative P&L is since
    inception, as a desk reports it: a window starting after the first trade
    starts from the total P&L at the previous close, so every window ends on
    the snapshot's total P&L."""
    if not pf.transactions:
        return [], []
    pf = with_contract_starts(pf)
    first = min(t.trade_date for t in pf.transactions)
    start = max(start, first)
    days = business_days(start, end)
    if not days:
        return [], []
    md = MarketData(start)
    by_id = {i.id: i for i in pf.instruments}
    warnings: List[str] = []
    prev = previous_business_day(days[0])
    prefetch_marks(pf, [prev] + days, md)
    prev_value = _portfolio_value(pf, prev, md, by_id)
    points: List[HistoryPoint] = []
    cumulative = snapshot(pf, prev).total_pnl if prev >= first else 0.0
    for d in days:
        value = _portfolio_value(pf, d, md, by_id)
        flows = sum(
            _flows_base(pf, t_id, day, md, _ccy(by_id[t_id], md))
            for t_id in {t.instrument_id for t in pf.transactions}
            for day in _days_between(prev, d)
        )
        if value is None or prev_value is None:
            pnl = None
        else:
            pnl = value - prev_value + flows
            cumulative += pnl
        points.append(
            HistoryPoint(d, value, pnl, cumulative if pnl is not None else None)
        )
        prev, prev_value = d, value
    if any(p.daily_pnl is None for p in points):
        warnings.append(
            "Some days have no P&L: a position could not be marked on them "
            "(no close stored, or a derivative past its expiry without a settlement trade)."
        )
    return points, warnings


def _portfolio_value(pf, d, md, by_id) -> Optional[float]:
    total = 0.0
    for iid, h in _holdings_at(pf, d).items():
        v = _value_base(by_id[iid], h.quantity, d, md, pf.base_currency)
        if v is None:
            return None
        total += v
    return total


# ── Methodology (shown on the page) ──────────────────────────────────────────

METHODOLOGY = [
    (
        "Positions come from the transactions",
        [
            "The list of transactions is the portfolio: a position is the sum of the "
            "trades on an instrument, so selling part of it reduces it and selling more "
            "than is held opens a short. Nothing edits a position except another trade.",
            "Cost is the weighted average cost (prix moyen pondéré): buying moves the "
            "average cost, selling realises (price − average cost) × quantity sold and "
            "leaves it unchanged. Fees are charged to realised P&L when paid. Realised + "
            "unrealised always equals market value plus sale proceeds minus purchase "
            "costs and fees.",
        ],
    ),
    (
        "Marking to market",
        [
            "Shares and indices are marked at their stored close on or before the "
            "valuation date, in their listing currency; a close more than five days old "
            "is flagged stale.",
            "Derivatives are fully revalued each day with that day's market: the "
            "underlying's close for the spot, the time left to expiry, the zero rate at "
            "that horizon from the currency's government curve (Rates tab), the stored "
            "dividend yield — and a model chosen per product, as an equity desk would. "
            "The i next to each position shows the model and every parameter it ran with.",
            "Where an option chain is stored for the underlying on that date (a US "
            "universe, recent dates), one SVI slice per maturity is fitted to it: the "
            "smile. Vanillas are marked at the smile's implied vol for their strike and "
            "maturity (Black-Scholes is then only the quoting convention; American "
            "options on a binomial tree). Digitals are the strike derivative of the "
            "vanilla on the smile: the flat-vol price corrected by vega × skew. Asians, "
            "barriers and lookbacks run on Dupire local volatility built from the same "
            "surface, which reprices every vanilla of the day.",
            "Without a stored chain there is no smile to fit: the volatility is the "
            "underlying's realised volatility over the last 63 business days — a proxy, "
            "shown as such. It is measured over elapsed time (squared log returns "
            "divided by the time they span), so a missing close makes one longer return "
            "rather than a false one-day jump. A quanto's FX volatility and asset/FX "
            "correlation are always historical: no FX options are stored.",
            "Asians, barriers and lookbacks are written as payoff scripts (Savine's "
            "scripting engine) observed at every business-day close from their start: "
            "the fixings already past are replayed from the stored closes, so a product "
            "that has started to live is priced from where it is — its partial average, "
            "the extremum reached, a barrier already crossed. They are priced by "
            "Monte-Carlo with the same draws every day, so the daily P&L is not noise; "
            "the standard error is shown with the model.",
            "What the market cannot supply keeps its trade-time value (status "
            "'default').",
            "An expired derivative is not valued past its expiry: book its settlement as "
            "a closing trade.",
        ],
    ),
    (
        "Currency and daily P&L",
        [
            "Values are converted to the portfolio's base currency at the ECB reference "
            "rate of the valuation date; realised P&L at the rate of the day it was "
            "realised. P&L in the base currency therefore includes the effect of FX moves.",
            "Daily P&L = value at the end of the day − value at the end of the previous "
            "business day + the day's cash flows (what trades paid or received). A past "
            "day's derivative marks are computed once and kept, like a desk's official "
            "end-of-day marks, not recomputed each time.",
        ],
    ),
]
