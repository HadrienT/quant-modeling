"""Marks of the rate and FX trades of a portfolio — interest rate swaps,
European and Bermudan swaptions, FX forwards and cross-currency swaps — on
the market of each day (blueprint/wp/23-xva.md, lot X12).

The market is the one the xVA page uses, read from the store with no
fallback: the SOFR (USD) and Euribor (EUR) swap curves and the swaption vols
built from the trades DTCC publishes, and the ECB reference rate of EUR/USD.
A date with no swap curve within a week is no mark, never an older one.

Conventions, those of the exposure engine (engines/xva): both legs of a swap
pay once a year, one curve per currency discounts and projects, a swap lasts
a whole number of years, and time runs on 365.25 days.

Two things are specific to a ledger:

- **Coupons already paid stay in the mark.** The ledger has no cash account:
  a swap that has just paid a coupon is worth that much less, and the P&L
  would show a loss that is only cash having changed hands. The mark of a
  swap is therefore the value of what is left to pay **plus the coupons
  paid since its start**, not compounded. The exposure of the trade (lot
  X13) counts only what is left.
- **The floating coupon of a period is set on its first day**, at the
  one-year rate of that day's curve. For Euribor that is the fixing; for
  SOFR, compounded in arrears, it is what the compounded rate was expected
  to be that day, and the realised compounding is not replayed.
"""

from __future__ import annotations

import bisect
import math
from datetime import date, timedelta
from typing import Any, Dict, List, Optional, Tuple

import quantmodeling as qm

from . import db, swaption_market
from .portfolio_marks import Input, Mark, ModelInfo, ModelParam
from .portfolio_schemas import RateFxSpec, add_years, whole_years

YEAR_DAYS = 365.25
#: What the DTCC sources of data-ingest call the swap of each currency.
SWAP_PRODUCT = {"USD": "NA/Swap OIS USD", "EUR": "NA/Swap Fxd Flt EUR"}
CURVE_NAME = {"USD": "SOFR swap curve", "EUR": "Euribor swap curve"}
#: A curve older than this is not the market of the day.
CURVE_STALE_DAYS = 7
#: Days of swaption trades behind a vol grid (as on the xVA page).
VOL_WINDOW_DAYS = 10
#: DTCC's files go back about two years (blueprint/wp/23-xva.md §13.7).
HISTORY_SINCE = date(2024, 9, 1)
#: A curve needs this many tenors to be bootstrapped.
_MIN_TENORS = 5
#: Fewer swaption vols than this cannot calibrate Hull-White (as on the xVA page).
MIN_CALIBRATION_VOLS = 3

Curve = Tuple[date, List[float], List[float]]


class NoMark(Exception):
    """Why a trade has no mark on a date: said on the position."""


def years(d: date, to: date) -> float:
    return (to - d).days / YEAR_DAYS


class RatesHistory:
    """The swap curves and swaption vols of each day, read once per currency
    and bootstrapped on demand."""

    def __init__(self) -> None:
        self._quotes: Dict[str, Tuple[List[date], Dict[date, list]]] = {}
        self._curves: Dict[Tuple[str, date], Curve] = {}
        self._trades: Dict[str, list] = {}
        self._vols: Dict[Tuple[str, date], List[Tuple[float, float, float]]] = {}
        self._hull_white: Dict[Tuple[str, date], Tuple[float, float]] = {}

    def _complete(self, ccy: str) -> Tuple[List[date], Dict[date, list]]:
        if ccy not in self._quotes:
            curves = db.dtcc_swap_curves(SWAP_PRODUCT[ccy], HISTORY_SINCE)
            complete = {d: r for d, r in curves.items() if len(r) >= _MIN_TENORS}
            self._quotes[ccy] = (sorted(complete), complete)
        return self._quotes[ccy]

    def curve(self, ccy: str, d: date) -> Curve:
        """The curve of the last day on or before `d` that has one."""
        dates, quotes = self._complete(ccy)
        i = bisect.bisect_right(dates, d) - 1
        if i < 0 or (d - dates[i]).days > CURVE_STALE_DAYS:
            raise NoMark(
                f"no {ccy} swap curve within {CURVE_STALE_DAYS} days of {d} in the "
                "store (data-ingest's dtcc-swap-rates source)"
            )
        obs = dates[i]
        if (ccy, obs) not in self._curves:
            built = qm.bootstrap_ois_curve(
                [], [(float(r.tenor_years), r.rate) for r in quotes[obs]]
            )
            self._curves[(ccy, obs)] = (
                obs,
                list(built["times"]),
                list(built["discount_factors"]),
            )
        return self._curves[(ccy, obs)]

    def fixing(self, ccy: str, d: date) -> float:
        """The one-year simple rate of the curve of `d`: the floating coupon
        of the period that starts that day."""
        _, times, dfs = self.curve(ccy, d)
        return 1.0 / discount(times, dfs, years(d, add_years(d, 1))) - 1.0

    def vols(self, ccy: str, obs: date) -> List[Tuple[float, float, float]]:
        """(expiry, tenor, ATM normal vol) from the swaptions reported in the
        window that ends on the curve date `obs`."""
        if (ccy, obs) not in self._vols:
            if ccy not in self._trades:
                self._trades[ccy] = db.dtcc_swaption_trades(
                    SWAP_PRODUCT[ccy], HISTORY_SINCE
                )
            start = obs - timedelta(days=VOL_WINDOW_DAYS)
            trades = [t for t in self._trades[ccy] if start <= t.report_date <= obs]
            grid = swaption_market.atm_normal_vols(trades, self._complete(ccy)[1])
            self._vols[(ccy, obs)] = [
                (p.expiry, float(p.tenor), p.normal_vol) for p in grid.points
            ]
        return self._vols[(ccy, obs)]

    def hull_white(self, ccy: str, obs: date) -> Tuple[float, float]:
        """(a, σ) of Hull-White fitted to the swaption vols of the curve date
        `obs`: about a second, done once per currency and date. What a
        Bermudan, which has no formula, is priced with."""
        if (ccy, obs) not in self._hull_white:
            vols = self.vols(ccy, obs)
            if len(vols) < MIN_CALIBRATION_VOLS:
                raise NoMark(
                    f"only {len(vols)} {ccy} swaption vol points in the "
                    f"{VOL_WINDOW_DAYS} days to {obs}: not enough to calibrate the "
                    "model a Bermudan is priced with"
                )
            _, times, dfs = self.curve(ccy, obs)
            fit = qm.calibrate_hull_white(times, dfs, times, dfs, vols, 1, 1, None)
            self._hull_white[(ccy, obs)] = (fit["mean_reversion"], fit["sigma"])
        return self._hull_white[(ccy, obs)]


def discount(times: List[float], dfs: List[float], t: float) -> float:
    return qm.rate_curve_discount_factors(times, dfs, [max(t, 0.0)])[0]


def _spot(md: Any, d: date) -> Tuple[date, float]:
    """USD per EUR, the ECB reference rate on or before `d`."""
    got = md.fx("EUR", "USD").at(d)
    if got is None or (d - got[0]).days > CURVE_STALE_DAYS:
        raise NoMark(f"no EUR/USD reference rate within a week of {d} in the store")
    return got[0], float(got[1])


def _status(d: date, observed: date) -> str:
    return "observed" if (d - observed).days <= 3 else "stale"


def schedule(spec: RateFxSpec) -> List[date]:
    """The yearly payment dates, the last one being the maturity. A swaption's
    are those of the swap it gives the right to enter."""
    return [
        add_years(spec.start, k)
        for k in range(1, whole_years(spec.start, spec.maturity) + 1)
    ]


# ── Each product ─────────────────────────────────────────────────────────────


def _annuity(curve: Curve, d: date, dates: List[date]) -> float:
    _, times, dfs = curve
    return sum(discount(times, dfs, years(d, p)) for p in dates)


def swap_value(
    spec: RateFxSpec, d: date, history: RatesHistory
) -> Tuple[float, float, Curve]:
    """(value of what is left to pay, coupons paid so far, the curve), for the
    side of the spec. Raises NoMark once nothing is left."""
    curve = history.curve(spec.currency, d)
    _, times, dfs = curve
    pay = schedule(spec)
    left = [p for p in pay if p > d]
    if not left:
        raise NoMark(
            f"matured on {spec.maturity}: book its settlement as a closing trade"
        )
    sign = 1.0 if spec.payer else -1.0
    fixed = spec.fixed_rate * _annuity(curve, d, left)
    last = discount(times, dfs, years(d, spec.maturity))
    if d < spec.start:
        floating = discount(times, dfs, years(d, spec.start)) - last
    else:
        reset = max([spec.start] + [p for p in pay if p <= d])
        floating = (1.0 + history.fixing(spec.currency, reset)) * discount(
            times, dfs, years(d, left[0])
        ) - last
    paid = sum(
        history.fixing(spec.currency, a) - spec.fixed_rate
        for a, b in zip([spec.start] + pay, pay)
        if b <= d
    )
    n = spec.notional
    return sign * n * (floating - fixed), sign * n * paid, curve


def par_rate(spec: RateFxSpec, d: date, history: RatesHistory) -> float:
    """The fixed rate that makes the swap (or a swaption's swap) worth zero."""
    curve = history.curve(spec.currency, d)
    _, times, dfs = curve
    floating = discount(times, dfs, years(d, spec.start)) - discount(
        times, dfs, years(d, spec.maturity)
    )
    return floating / _annuity(curve, d, schedule(spec))


def _mark_swap(spec: RateFxSpec, d: date, md: Any) -> Mark:
    value, paid, (obs, _, _) = swap_value(spec, d, md.rates_history)
    name = CURVE_NAME[spec.currency]
    return Mark(
        value + paid,
        spec.currency,
        [
            Input(name, _status(d, obs), None, obs),
            Input("value of the coupons left", "observed", value, d),
            Input("coupons paid since the start", "observed", paid, d),
        ],
        model=ModelInfo(
            "Discounted cash flows, one curve",
            "closed form",
            "A swap is a strip of known and forward coupons: it is valued on the "
            "swap curve of the day, which both discounts and projects. The coupons "
            "already paid stay in the mark, because the ledger has no cash account.",
            [
                ModelParam("fixed rate", spec.fixed_rate, status="contract"),
                ModelParam("notional", spec.notional, status="contract"),
                ModelParam(name, text=obs.isoformat(), status=_status(d, obs)),
            ],
        ),
    )


def _mark_swaption(spec: RateFxSpec, d: date, md: Any) -> Mark:
    if d >= spec.start:
        raise NoMark(
            f"expired on {spec.start}: book its settlement as a closing trade, and "
            "the swap entered, if it was exercised, as a new one"
        )
    history: RatesHistory = md.rates_history
    obs, times, dfs = history.curve(spec.currency, d)
    expiry, tenor = years(d, spec.start), whole_years(spec.start, spec.maturity)
    grid = history.vols(spec.currency, obs)
    if not grid:
        raise NoMark(
            f"no {spec.currency} swaption traded in the {VOL_WINDOW_DAYS} days to "
            f"{obs}: no volatility to price it with"
        )
    e, t, vol = min(grid, key=lambda v: (v[0] - expiry) ** 2 + (v[1] - tenor) ** 2)
    out = qm.price_swaption(
        times,
        dfs,
        times,
        dfs,
        expiry,
        float(tenor),
        spec.fixed_rate,
        spec.payer,
        1,
        1,
        spec.notional,
        normal_vol=vol,
    )
    point = f"{e:g}Y into {t:g}Y"
    name = CURVE_NAME[spec.currency]
    return Mark(
        float(out["bachelier"]),
        spec.currency,
        [
            Input(name, _status(d, obs), None, obs),
            Input("forward swap rate", "observed", float(out["forward"]), obs),
            Input(f"normal vol ({point} at the money)", "proxied", vol, obs),
        ],
        model=ModelInfo(
            "Bachelier (normal)",
            "closed form",
            "A European swaption is quoted in normal volatility. The vols come from "
            "traded at-the-money swaptions and carry no smile: the nearest point of "
            "the day's grid is used whatever the strike.",
            [
                ModelParam("strike", spec.fixed_rate, status="contract"),
                ModelParam("forward swap rate", float(out["forward"])),
                ModelParam(f"normal vol, {point}", vol, status="proxied"),
                ModelParam(name, text=obs.isoformat(), status=_status(d, obs)),
            ],
        ),
    )


def bermudan_exercises(spec: RateFxSpec, d: date) -> List[date]:
    """The exercise dates still ahead on `d`: the first one and each
    anniversary while a full year of the swap is left."""
    n = whole_years(spec.start, spec.maturity)
    return [e for e in (add_years(spec.start, k) for k in range(n)) if e > d]


def _mark_bermudan(spec: RateFxSpec, d: date, md: Any) -> Mark:
    left = bermudan_exercises(spec, d)
    if not left:
        raise NoMark(
            f"its last exercise date, {spec.end}, has passed: book its settlement "
            "as a closing trade, and the swap entered, if it was exercised, as a "
            "new one"
        )
    history: RatesHistory = md.rates_history
    obs, times, dfs = history.curve(spec.currency, d)
    a, sigma = history.hull_white(spec.currency, obs)
    first = years(d, left[0])
    tenor = whole_years(left[0], spec.maturity)
    out = qm.price_swaption(
        times,
        dfs,
        times,
        dfs,
        first,
        float(tenor),
        spec.fixed_rate,
        spec.payer,
        1,
        1,
        spec.notional,
        hull_white=(a, sigma),
        bermudan_exercises=[first + k for k in range(len(left))],
    )
    name = CURVE_NAME[spec.currency]
    return Mark(
        float(out["hull_white_bermudan"]),
        spec.currency,
        [
            Input(name, _status(d, obs), None, obs),
            Input("Hull-White mean reversion", "proxied", a, obs),
            Input("Hull-White volatility", "proxied", sigma, obs),
            Input("exercise dates left", "observed", float(len(left)), d),
        ],
        model=ModelInfo(
            "Hull-White, one factor",
            "lattice",
            "A Bermudan can be exercised on several dates and has no formula: it "
            "is priced on a lattice of the short rate, under Hull-White fitted that "
            "day to the traded at-the-money swaption vols. If its first exercise "
            "date has passed without a closing trade, it is valued as still alive "
            "on the dates that are left.",
            [
                ModelParam("strike", spec.fixed_rate, status="contract"),
                ModelParam("mean reversion", a, status="calibrated"),
                ModelParam("volatility", sigma, status="calibrated"),
                ModelParam("European at the first date", float(out["hull_white"])),
                ModelParam(name, text=obs.isoformat(), status=_status(d, obs)),
            ],
        ),
    )


def _two_curves(d: date, md: Any) -> Tuple[Curve, Curve, date, float]:
    history: RatesHistory = md.rates_history
    spot_date, spot = _spot(md, d)
    return history.curve("USD", d), history.curve("EUR", d), spot_date, spot


def _fx_inputs(d: date, usd: Curve, eur: Curve, spot_date: date, spot: float):
    return [
        Input("EUR/USD (ECB reference rate)", _status(d, spot_date), spot, spot_date),
        Input(CURVE_NAME["USD"], _status(d, usd[0]), None, usd[0]),
        Input(CURVE_NAME["EUR"], _status(d, eur[0]), None, eur[0]),
    ]


def forward_rate(d: date, maturity: date, md: Any) -> float:
    """USD per EUR for delivery at `maturity`, by covered interest parity."""
    usd, eur, _, spot = _two_curves(d, md)
    t = years(d, maturity)
    return spot * discount(eur[1], eur[2], t) / discount(usd[1], usd[2], t)


def _mark_fx_forward(spec: RateFxSpec, d: date, md: Any) -> Mark:
    if d >= spec.maturity:
        raise NoMark(
            f"delivered on {spec.maturity}: book its settlement as a closing trade"
        )
    usd, eur, spot_date, spot = _two_curves(d, md)
    t = years(d, spec.maturity)
    sign = 1.0 if spec.payer else -1.0
    value = (
        sign
        * spec.notional
        * (
            spot * discount(eur[1], eur[2], t)
            - spec.fixed_rate * discount(usd[1], usd[2], t)
        )
    )
    forward = spot * discount(eur[1], eur[2], t) / discount(usd[1], usd[2], t)
    return Mark(
        value,
        "USD",
        _fx_inputs(d, usd, eur, spot_date, spot)
        + [Input("forward EUR/USD", "observed", forward, d)],
        model=ModelInfo(
            "Covered interest parity",
            "closed form",
            "An FX forward is two known payments, each discounted on its own "
            "currency's swap curve and the euro one converted at the spot rate. No "
            "cross-currency basis is added: none is stored.",
            [
                ModelParam("delivery rate", spec.fixed_rate, status="contract"),
                ModelParam("EUR notional", spec.notional, status="contract"),
                ModelParam("forward EUR/USD", forward),
            ],
        ),
    )


def cross_currency_value(
    spec: RateFxSpec, d: date, md: Any
) -> Tuple[float, float, list]:
    """(value in dollars of what is left, coupons paid so far in dollars,
    inputs), for the side of the spec."""
    usd, eur, spot_date, spot = _two_curves(d, md)
    pay = schedule(spec)
    left = [p for p in pay if p > d]
    if not left:
        raise NoMark(
            f"matured on {spec.maturity}: book its settlement as a closing trade"
        )
    t = years(d, spec.maturity)
    euros = spec.notional * (
        (spec.foreign_rate or 0.0) * _annuity(eur, d, left)
        + discount(eur[1], eur[2], t)
    )
    dollars = (spec.domestic_notional or 0.0) * (
        spec.fixed_rate * _annuity(usd, d, left) + discount(usd[1], usd[2], t)
    )
    sign = 1.0 if spec.payer else -1.0
    paid = sum(
        spec.notional * (spec.foreign_rate or 0.0) * _spot(md, p)[1]
        - (spec.domestic_notional or 0.0) * spec.fixed_rate
        for p in pay
        if p <= d
    )
    return (
        sign * (spot * euros - dollars),
        sign * paid,
        _fx_inputs(d, usd, eur, spot_date, spot),
    )


def _mark_cross_currency(spec: RateFxSpec, d: date, md: Any) -> Mark:
    value, paid, inputs = cross_currency_value(spec, d, md)
    return Mark(
        value + paid,
        "USD",
        inputs
        + [
            Input("value of the flows left", "observed", value, d),
            Input("coupons paid since the start", "observed", paid, d),
        ],
        model=ModelInfo(
            "Discounted cash flows, two curves",
            "closed form",
            "Fixed coupons in each currency and the notionals exchanged back at "
            "maturity, each leg discounted on its own swap curve and the euro leg "
            "converted at the spot rate. The coupons already paid stay in the mark, "
            "converted at the rate of the day they were paid.",
            [
                ModelParam("EUR coupon", spec.foreign_rate, status="contract"),
                ModelParam("USD coupon", spec.fixed_rate, status="contract"),
                ModelParam("EUR notional", spec.notional, status="contract"),
                ModelParam("USD notional", spec.domestic_notional, status="contract"),
            ],
        ),
    )


_MARKS = {
    "swap": _mark_swap,
    "swaption": _mark_swaption,
    "bermudan": _mark_bermudan,
    "fx_forward": _mark_fx_forward,
    "cross_currency_swap": _mark_cross_currency,
}


# ── Sensitivities ────────────────────────────────────────────────────────────

#: One basis point: the move of a curve and of a normal volatility.
BP = 1e-4
#: The relative move of the exchange rate the FX delta is measured on.
SPOT_MOVE = 0.01


class _Moved:
    """The market of a day with one thing moved: the swap curve of a
    currency shifted in parallel (every zero rate by `curve` ), the swaption
    vols by `vol`, or the exchange rate by a factor. Only the day's own
    market moves: a coupon fixed on an earlier curve keeps its fixing."""

    def __init__(self, md: Any, d: date, ccy: str = "", curve=0.0, vol=0.0, spot=1.0):
        self._md, self._d, self._ccy = md, d, ccy
        self._curve, self._vol, self._spot = curve, vol, spot
        self.rates_history = self

    # The part of RatesHistory the marks read.
    def curve(self, ccy: str, d: date) -> Curve:
        base: RatesHistory = self._md.rates_history
        obs, times, dfs = base.curve(ccy, d)
        if ccy != self._ccy or obs != base.curve(ccy, self._d)[0]:
            return obs, times, dfs
        return obs, times, [f * math.exp(-self._curve * t) for f, t in zip(dfs, times)]

    def fixing(self, ccy: str, d: date) -> float:
        _, times, dfs = self.curve(ccy, d)
        return 1.0 / discount(times, dfs, years(d, add_years(d, 1))) - 1.0

    def hull_white(self, ccy: str, obs: date) -> Tuple[float, float]:
        # The model is held as calibrated: a Bermudan's PV01 is at fixed
        # Hull-White parameters.
        return self._md.rates_history.hull_white(ccy, obs)

    def vols(self, ccy: str, obs: date) -> List[Tuple[float, float, float]]:
        return [
            (e, t, v + self._vol) for e, t, v in self._md.rates_history.vols(ccy, obs)
        ]

    # The part of MarketData they read.
    def fx(self, ccy: str, base: str) -> Any:
        series, d, factor = self._md.fx(ccy, base), self._d, self._spot

        class _Series:
            def at(self, when: date):
                got = series.at(when)
                if got is None or when != d:
                    return got
                return got[0], got[1] * factor

        return _Series()


def sensitivities(spec: RateFxSpec, d: date, md: Any, value: float) -> Dict[str, float]:
    """What the mark moves by, by bumping and revaluing (the marks are
    closed forms), in the currency of the mark:

    - `pv01_usd`, `pv01_eur`: for one basis point more on every zero rate of
      that currency's swap curve;
    - `fx_delta`: the euros the trade is long of — the move of its value for
      a move of EUR/USD, per dollar of that move;
    - `normal_vega`: for one basis point more of normal volatility.

    Only what the product depends on is there. A Bermudan has its PV01
    at fixed Hull-White parameters, and no vega: moving a quoted vol moves a
    calibration, which is a second a move.
    """
    price = _MARKS[spec.product]
    out: Dict[str, float] = {}
    fx = spec.product in ("fx_forward", "cross_currency_swap")
    for ccy in ("USD", "EUR") if fx else (spec.currency,):
        moved = price(spec, d, _Moved(md, d, ccy, curve=BP)).value
        out[f"pv01_{ccy.lower()}"] = moved - value
    if fx:
        _, spot = _spot(md, d)
        moved = price(spec, d, _Moved(md, d, spot=1.0 + SPOT_MOVE)).value
        out["fx_delta"] = (moved - value) / (SPOT_MOVE * spot)
    if spec.product == "swaption":
        out["normal_vega"] = price(spec, d, _Moved(md, d, vol=BP)).value - value
    return out


def mark(spec: RateFxSpec, d: date, md: Any) -> Mark:
    """The mark of one contract on `d`, in `spec.value_currency`, with its
    sensitivities; a Mark with no value and its reason when the store cannot
    back it."""
    try:
        m = _MARKS[spec.product](spec, d, md)
        if m.value is not None and math.isfinite(m.value):
            m.greeks = sensitivities(spec, d, md, m.value)
    except NoMark as exc:
        return Mark(None, spec.value_currency, note=str(exc))
    except Exception as exc:  # noqa: BLE001 — one position must not sink the book
        return Mark(None, spec.value_currency, note=f"pricing failed: {exc}")
    if m.value is None or not math.isfinite(m.value):
        return Mark(None, spec.value_currency, note="pricing gave no finite value")
    return m


# ── Terms at the market, for the trade form ──────────────────────────────────


def at_market(draft: dict, d: date, md: Any) -> RateFxSpec:
    """The contract with every rate the draft leaves out set at the market of
    `d`: a swap at its par rate, a swaption at the money, a forward at the
    forward rate, a cross-currency swap with both coupons at par and its
    dollar notional at the spot rate. Raises NoMark when the store has no
    market for that day."""
    terms = dict(draft)
    product = terms.get("product")
    # A stand-in rate, to build the contract the par rate is computed on.
    probe = RateFxSpec.model_validate(
        {
            **terms,
            "fixed_rate": terms.get("fixed_rate") or 0.0,
            "foreign_rate": terms.get("foreign_rate") or 0.0,
            "domestic_notional": terms.get("domestic_notional") or 1.0,
        }
    )
    history: RatesHistory = md.rates_history
    if product in ("swap", "swaption", "bermudan"):
        if terms.get("fixed_rate") is None:
            terms["fixed_rate"] = par_rate(probe, d, history)
    elif product == "fx_forward":
        if terms.get("fixed_rate") is None:
            terms["fixed_rate"] = forward_rate(d, probe.maturity, md)
    else:
        usd, eur, _, spot = _two_curves(d, md)
        pay = schedule(probe)

        def par_coupon(curve: Curve) -> float:
            last = discount(curve[1], curve[2], years(d, probe.maturity))
            return (1.0 - last) / _annuity(curve, d, pay)

        if terms.get("fixed_rate") is None:
            terms["fixed_rate"] = par_coupon(usd)
        if terms.get("foreign_rate") is None:
            terms["foreign_rate"] = par_coupon(eur)
        if terms.get("domestic_notional") is None:
            terms["domestic_notional"] = probe.notional * spot
    return RateFxSpec.model_validate(terms)


def label(spec: RateFxSpec) -> str:
    n = f"{spec.notional / 1e6:g}M"
    if spec.product == "swap":
        side = "Payer" if spec.payer else "Receiver"
        return (
            f"{side} swap {spec.currency} {n} at {spec.fixed_rate:.3%}, "
            f"{spec.start} to {spec.maturity}"
        )
    if spec.product == "swaption":
        side = "Payer" if spec.payer else "Receiver"
        return (
            f"{side} swaption {spec.currency} {n} struck at {spec.fixed_rate:.3%}, "
            f"expiring {spec.start}, swap to {spec.maturity}"
        )
    if spec.product == "bermudan":
        side = "Payer" if spec.payer else "Receiver"
        return (
            f"Bermudan {side.lower()} swaption {spec.currency} {n} struck at "
            f"{spec.fixed_rate:.3%}, yearly from {spec.start}, swap to {spec.maturity}"
        )
    if spec.product == "fx_forward":
        side = "Buy" if spec.payer else "Sell"
        return f"{side} EUR {n} at {spec.fixed_rate:.4f} USD, delivery {spec.maturity}"
    side = "receive EUR, pay USD" if spec.payer else "pay EUR, receive USD"
    return f"Cross-currency swap EUR {n}, {side}, to {spec.maturity}"


__all__ = [
    "NoMark",
    "RatesHistory",
    "at_market",
    "bermudan_exercises",
    "cross_currency_value",
    "label",
    "mark",
    "par_rate",
    "schedule",
    "sensitivities",
    "swap_value",
    "years",
]
