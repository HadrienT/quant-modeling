"""Script positions of a portfolio (issue 313, lot b3; blueprint/wp/26-
performance.md): products of the script library, booked with their terms
and marked each day of their life.

**Booking.** The product's script is rendered with its terms on the trade
date (product_templates.render, lead_days = 0: the first date falls in the
week that starts on the trade date, whole weeks keeping every date on its
weekday) and stored in the position (portfolio_schemas.ScriptSpec). Its
dates are then the contract's: a later mark replays them, it does not move
them.

**Marks, one per day** (portfolio_valuation, stored once):

- *Past dates are replayed.* An equity script's events on or before the
  day are fixed on the underlying's stored closes (Savine's engine, lot
  16e); a past exercise date is a right that was not used (an exercise is
  booked as a closing trade). A rates script reads no stock: its past events
  that read the curve (df(), fwdRate()) or decide an exercise are taken out
  of the script, their term assignments kept (`season`).
- *Flows already paid stay in the mark.* The ledger has no cash account: a
  product that has paid a coupon would otherwise show the coupon as a loss.
  The mark is the value of what is left **plus what was paid since the
  start**, not compounded -- the convention of the rate trades
  (portfolio_rates.py).
- *Closed form where there is one*, the product's own oracle: a position
  converted from a hand-written pricer is marked by it (`closed_form`, the
  desk policy of portfolio_models.py); the European, the digital and the
  forward of the library once their strike is fixed on their first date
  (Black-Scholes at the smile's vol, cost of carry); bonds and swaps on the
  day's curve (no rate volatility enters a linear payoff); the European
  swaption by Hull-White's closed form on the day's SOFR curve and the
  day's calibration.
- *Monte-Carlo otherwise*, on the model the desk policy chooses for the day
  (stochastic-local vol, local vol, Black-Scholes at the realised-vol proxy
  without a smile; Hull-White calibrated to the day's swaptions for a rates
  script), one seed for every day so that the daily P&L is not simulation
  noise.

Not covered: scripts on several underlyings and quantos (no correlated
history is stored for them yet); they are refused at booking.
"""

from __future__ import annotations

import math
import re
from datetime import date, timedelta
from functools import lru_cache
from typing import Any, Dict, List, Optional, Tuple

import quantmodeling as qm

from . import portfolio_rates, product_templates
from .portfolio_marks import Input, Mark, ModelInfo, ModelParam
from .portfolio_schemas import DerivativeSpec, ScriptSpec

#: Monte-Carlo of a script position: as portfolio_models (the same draws
#: every day).
PATHS = 20_000
SEED = 7
#: Hull-White on a government curve: a bond's value does not depend on it.
GOVERNMENT_HW = (0.03, 0.01)
#: Library products marked by a closed form once their first date has fixed
#: their strike: the hand-written pricer of the same payoff.
FIXED_STRIKE = {
    "european-call": "vanilla",
    "digital-call": "digital",
    "equity-forward": "future",
}
#: Rates products whose value is linear in the curve: the script valued with
#: no rate volatility is their value on the curve.
LINEAR_RATES = {"zero-coupon-bond", "fixed-rate-bond", "interest-rate-swap"}

_ISO = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_CONSTANT = re.compile(r"^\s+\w+ = [-+]?[0-9.]+(?:[eE][-+]?\d+)?\s*$")
_READS_CURVE = re.compile(r"\b(df|fwdRate|fwd|exercise|call)\(")


class BookingError(ValueError):
    """A product the portfolio cannot hold as booked."""


# ── Booking ──────────────────────────────────────────────────────────────────


@lru_cache(maxsize=512)
def event_dates(script: str, start: date) -> Tuple[date, ...]:
    """Every event date of a script, schedules resolved, seen from the day
    before its first date (so that none is historical yet)."""
    events = qm.validate_script(script, (start - timedelta(days=1)).isoformat())[
        "events"
    ]
    return tuple(date.fromisoformat(e["date"]) for e in events)


def book(
    product: str,
    terms: Dict[str, float],
    trade_date: date,
    underlying: Optional[str] = None,
    currency: Optional[str] = None,
) -> ScriptSpec:
    """The position of a library product booked on `trade_date`."""
    t = product_templates.get(product)
    if t.underlyings > 1 or t.quanto:
        raise BookingError(
            f"'{t.title}' is on several underlyings or a quanto: a portfolio holds "
            "single-underlying scripts and rates scripts for now (no correlated "
            "history is stored for the others)"
        )
    script = product_templates.render(product, terms, trade_date, lead_days=0)
    first = min(date.fromisoformat(d) for d in product_templates._ISO.findall(script))
    dates = event_dates(script, first)
    rates = t.market == "rates"
    if rates:
        if underlying:
            raise BookingError(f"'{t.title}' is a rates product: it takes no ticker")
        ccy = "USD" if t.curve == "sofr" else (currency or "USD")
        if t.curve == "sofr" and ccy != "USD":
            raise BookingError(f"'{t.title}' is on the USD SOFR curve")
    else:
        if not underlying:
            raise BookingError(f"'{t.title}' needs its underlying's ticker")
        ccy = currency or "USD"
    return ScriptSpec(
        product=product,
        title=t.title,
        terms={p.name: float(terms.get(p.name, p.default)) for p in t.params},
        script=script,
        underlying=None if rates else underlying.upper(),
        currency=ccy,
        start=dates[0],
        expiry=dates[-1],
        curve=t.curve if rates else None,
    )


def label(spec: ScriptSpec) -> str:
    who = spec.underlying or (
        "USD SOFR" if spec.curve == "sofr" else f"{spec.currency} curve"
    )
    return f"{who} {spec.title or spec.product} {spec.expiry.isoformat()}"


# ── Seasoning (rates scripts) ────────────────────────────────────────────────


def _blocks(script: str) -> List[Tuple[str, List[str]]]:
    """(header, body lines) of each event block: a date or a schedule()."""
    out: List[Tuple[str, List[str]]] = []
    for line in script.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if not line[0].isspace():
            out.append((line.strip(), []))
        elif out:
            out[-1][1].append(line)
    return out


def season(script: str, d: date) -> Tuple[str, List[Tuple[date, str]]]:
    """A rates script as it stands on `d`: each past dated event that reads
    the curve or decides an exercise is taken out -- a coupon already fixed,
    a right not used -- its constant assignments (the terms) kept on its
    date. Returns the script and the events taken out that paid something
    (date, the event as a script with the terms before it), whose amounts
    the mark adds back."""
    kept: List[str] = []
    paid: List[Tuple[date, str]] = []
    constants: List[str] = []
    for header, body in _blocks(script):
        dated = _ISO.match(header) is not None
        past = dated and date.fromisoformat(header) <= d
        if past and any(_READS_CURVE.search(b) for b in body):
            consts = [b for b in body if _CONSTANT.match(b)]
            if any("pays" in b for b in body):
                paid.append(
                    (
                        date.fromisoformat(header),
                        "\n".join([header] + constants + body) + "\n",
                    )
                )
            if consts:
                kept.append("\n".join([header] + consts))
            constants += consts
            continue
        if not dated and any(_READS_CURVE.search(b) for b in body):
            # a schedule that reads the curve could be partly past: no
            # library product writes one
            if date.fromisoformat(header.split("(")[1].split(",")[0].strip()) <= d:
                raise ValueError(
                    "a past schedule() that reads the curve cannot be seasoned"
                )
        kept.append("\n".join([header] + body))
        constants += [b for b in body if _CONSTANT.match(b)]
    return "\n\n".join(kept) + "\n", paid


def for_exposure(script: str, d: date) -> Optional[str]:
    """The script with nothing on or before `d`, for the exposure engine
    (which replays no fixing): the terms of the past events carried into the
    first future one. None when nothing is left."""
    head: List[str] = []
    out: List[str] = []
    for header, body in _blocks(script):
        dated = _ISO.match(header) is not None
        if dated and date.fromisoformat(header) <= d:
            head += [b for b in body if _CONSTANT.match(b)]
            continue
        if not dated:
            raise ValueError(
                "a schedule() in a rates script is not supported by the exposure engine"
            )
        out.append("\n".join([header] + (head if not out else []) + body))
    return "\n\n".join(out) + "\n" if out else None


# ── Marks ────────────────────────────────────────────────────────────────────


def mark(spec: ScriptSpec, d: date, md: Any, today: Optional[date] = None) -> Mark:
    """The mark of one script position on `d`, in its currency."""
    if d >= spec.expiry:
        return Mark(
            None,
            spec.currency,
            note=f"expired on {spec.expiry}: book its settlement as a closing trade",
        )
    try:
        if spec.curve is not None:
            return _mark_rates(spec, d, md)
        return _mark_equity(spec, d, md, today or date.today())
    except portfolio_rates.NoMark as exc:
        return Mark(None, spec.currency, note=str(exc))
    except Exception as exc:  # noqa: BLE001 — one position must not sink the book
        return Mark(None, spec.currency, note=f"pricing failed: {exc}")


def _mark_equity(spec: ScriptSpec, d: date, md: Any, today: date) -> Mark:
    from .portfolio_valuation import _mark_derivative

    T = max((spec.expiry - d).days / 365.25, 1e-6)
    if spec.closed_form is not None:
        return _mark_derivative(spec.closed_form, d, md, today)
    kind = FIXED_STRIKE.get(spec.product)
    if kind is not None and d >= spec.start:
        got = md.closes_from(spec.underlying, spec.start).at(spec.start)
        if got is not None:
            s0 = float(got[1])
            m = _mark_derivative(_fixed_strike_view(spec, kind, s0, T), d, md, today)
            if m.model is not None:
                m.model.params.insert(
                    0,
                    ModelParam(
                        "initial level s0",
                        s0,
                        status="observed" if got[0] == spec.start else "stale",
                        source=f"{spec.underlying} close of {got[0]}, the script's first date",
                    ),
                )
            return m
    view = DerivativeSpec(
        product="script",
        params={
            "spot": 1.0,
            "rate": 0.0,
            "dividend": 0.0,
            "vol": 0.2,
            "maturity": T,
            "start_date": spec.start.isoformat(),
            "script": spec.script,
            "title": spec.title or spec.product,
        },
        underlying=spec.underlying,
        expiry=spec.expiry,
        currency=spec.currency,
    )
    return _mark_derivative(view, d, md, today)


def _fixed_strike_view(
    spec: ScriptSpec, kind: str, s0: float, T: float
) -> DerivativeSpec:
    k = float(spec.terms.get("strike", 1.0))
    base = {"spot": s0, "maturity": T, "rate": 0.0, "dividend": 0.0, "strike": k * s0}
    if kind == "vanilla":
        params = {**base, "vol": 0.2, "is_call": True, "engine": "analytic"}
    elif kind == "digital":
        params = {
            **base,
            "vol": 0.2,
            "is_call": True,
            "payoff_type": "cash_or_nothing",
            "cash_amount": float(spec.terms.get("payout", 1.0)),
        }
    else:
        params = {**base, "notional": float(spec.terms.get("notional", 1.0))}
    return DerivativeSpec(
        product=kind,
        params=params,
        underlying=spec.underlying,
        expiry=spec.expiry,
        currency=spec.currency,
    )


def _curve_of(spec: ScriptSpec, d: date, md: Any):
    """(curve date, times, discount factors, a, sigma, source) of the day."""
    if spec.curve == "sofr":
        hist: portfolio_rates.RatesHistory = md.rates_history
        obs, times, dfs = hist.curve("USD", d)
        a, sigma = hist.hull_white("USD", obs)
        return obs, times, dfs, a, sigma, f"USD SOFR swap curve of {obs} (DTCC)"
    got = md.curve(spec.currency).at(d)
    if got is None:
        raise portfolio_rates.NoMark(
            f"no {spec.currency} government curve on or before {d}"
        )
    obs, (times, dfs) = got
    a, sigma = GOVERNMENT_HW
    return (
        obs,
        list(times),
        list(dfs),
        a,
        sigma,
        f"{spec.currency} government curve of {obs}",
    )


def _hw_price(
    script: str, d: date, curve, sigma: float, paths: int, fixings=None
) -> dict:
    _, times, dfs, a, _, _ = curve
    return qm.price_script(
        script,
        1.0,
        0.0,
        0.0,
        1e-12,
        d.isoformat(),
        n_paths=paths,
        seed=SEED,
        model="hull_white",
        hull_white={"mean_reversion": a, "sigma": sigma, "rho": 0.0},
        curve_times=list(times),
        curve_dfs=list(dfs),
        historical_fixings=fixings or {},
    )


_PAID: Dict[Tuple[str, date], float] = {}


def _paid_amount(spec: ScriptSpec, when: date, event: str, md: Any) -> float:
    """What a past event that read the curve paid, on the curve of its own
    date: the event valued the day before on that curve with no rate
    volatility (its flows deterministic), grown back over the day."""
    key = (spec.script, when)
    if key not in _PAID:
        curve = _curve_of(spec, when, md)
        eve = when - timedelta(days=1)
        res = _hw_price(event, eve, curve, 1e-12, 1_000)
        _, times, dfs = curve[:3]
        [day] = qm.rate_curve_discount_factors(list(times), list(dfs), [1.0 / 365.0])
        _PAID[key] = float(res["npv"]) / day
    return _PAID[key]


def _mark_rates(spec: ScriptSpec, d: date, md: Any) -> Mark:
    curve = _curve_of(spec, d, md)
    obs, times, dfs, a, sigma, source = curve
    script, taken_out = season(spec.script, d)
    fixings = {
        e.isoformat(): [1.0] for e in event_dates(spec.script, spec.start) if e <= d
    }
    paid = sum(_paid_amount(spec, when, ev, md) for when, ev in taken_out)
    std_error: Optional[float] = None
    if spec.product == "european-swaption":
        value, engine = (
            _swaption_closed_form(spec, d, times, dfs, a, sigma),
            "closed form",
        )
        paid_replay = 0.0
    else:
        linear = spec.product in LINEAR_RATES
        res = _hw_price(
            script,
            d,
            curve,
            1e-12 if linear else sigma,
            1_000 if linear else PATHS,
            fixings,
        )
        value = float(res["npv"])
        paid_replay = float(res.get("historical_payments", 0.0))
        engine = (
            "the curve (no rate volatility enters a linear payoff)"
            if linear
            else "Monte-Carlo, Longstaff-Schwartz"
        )
        if not linear:
            std_error = float(res["mc_std_error"])
    paid += paid_replay
    hw = [
        ModelParam(
            "mean reversion a",
            a,
            status="calibrated" if spec.curve == "sofr" else "default",
            source=source,
        ),
        ModelParam(
            "short-rate vol σ",
            sigma,
            status="calibrated" if spec.curve == "sofr" else "default",
            source=(
                "Hull-White fitted to the ATM swaption vols of the day's trades"
                if spec.curve == "sofr"
                else "a bond's value does not depend on it"
            ),
        ),
    ]
    params = hw + [ModelParam(k, v, status="contract") for k, v in spec.terms.items()]
    if paid:
        params.append(
            ModelParam(
                "flows already paid",
                paid,
                status="observed",
                source="kept in the mark, not compounded: the ledger has no cash account",
            )
        )
    return Mark(
        value + paid,
        spec.currency,
        [
            Input(
                f"curve ({source})",
                "observed" if (d - obs).days <= 7 else "stale",
                None,
                obs,
            )
        ],
        model=ModelInfo(
            "Hull-White on the "
            + ("SOFR curve" if spec.curve == "sofr" else "government curve"),
            f"the product's script: {engine}",
            "A rates script is valued on the day's curve, its short rate calibrated to the day's "
            "swaptions; past coupons are replayed on the curve of their own date.",
            params,
            std_error=std_error,
        ),
    )


def _swaption_closed_form(
    spec: ScriptSpec, d: date, times, dfs, a: float, sigma: float
) -> float:
    """Hull-White's closed form on the script's own dates (the oracle of its
    Monte-Carlo, pricing_service._price_sofr_closed_form)."""
    dates = event_dates(spec.script, spec.start)
    expiry = dates[-1]
    paid = sorted(
        {
            date.fromisoformat(x)
            for x in re.findall(r"df\((\d{4}-\d{2}-\d{2})\)", spec.script)
        }
    )
    probe = "".join(f"{x.isoformat()}\n    x = 0\n" for x in [expiry] + paid)
    t = [e["t"] for e in qm.validate_script(probe, d.isoformat())["events"]]
    periods = [(s, e, e - s) for s, e in zip(t[:-1], t[1:])]
    side = float(spec.terms.get("side", 1.0))
    out = qm.hull_white_swap_on_dates(
        list(times),
        list(dfs),
        a,
        sigma,
        periods,
        float(spec.terms["strike"]),
        side > 0,
        float(spec.terms["notional"]) * abs(side),
        [t[0]],
    )
    return float(out["european"])


# ── Converted hand-written positions ─────────────────────────────────────────


def _european(K: float, call: bool, expiry: date) -> str:
    payoff = f"max(spot() - {K!r}, 0)" if call else f"max({K!r} - spot(), 0)"
    return f"{expiry.isoformat()}\n    pays {payoff}\n"


def from_hand_written(spec: DerivativeSpec, opened: date) -> Optional[ScriptSpec]:
    """A hand-written derivative position as a script of the same payoff,
    the hand-written terms kept as its closed form (`closed_form`: the mark
    is the one it had). None for a product with no script writer here."""
    from . import scripted_payoffs

    p = spec.params
    call = bool(p.get("is_call", True))
    K = float(p.get("strike", 0.0))
    side = "call" if call else "put"
    if spec.product == "vanilla":
        script, title = _european(K, call, spec.expiry), f"European {side}"
    elif spec.product == "american_vanilla":
        payoff = f"max(spot() - {K!r}, 0)" if call else f"max({K!r} - spot(), 0)"
        cond = f"spot() > {K!r}" if call else f"spot() < {K!r}"
        last = spec.expiry - timedelta(days=7)
        script = (
            f"{opened.isoformat()}\n    done = 0\n\n"
            f"schedule({(opened + timedelta(days=7)).isoformat()}, {last.isoformat()}, 1W, US, F)\n"
            f"    if done = 0 and {cond} and exercise(spot()) then\n"
            f"        pays {payoff}\n        done = 1\n    endIf\n\n"
            f"{spec.expiry.isoformat()}\n    if done = 0 then\n        pays {payoff}\n    endIf\n"
        )
        title = f"American {side} (weekly exercise)"
    elif spec.product == "digital":
        cash = float(p.get("cash_amount", 1.0))
        cond = f"spot() > {K!r}" if call else f"spot() < {K!r}"
        script = f"{spec.expiry.isoformat()}\n    if {cond} then pays {cash!r} endIf\n"
        title = f"Digital {side}"
    elif spec.product == "future":
        n = float(p.get("notional", 1.0))
        script = f"{spec.expiry.isoformat()}\n    pays {n!r} * (spot() - {K!r})\n"
        title = "Forward"
    elif spec.product == "quanto":
        script, title = _european(K, call, spec.expiry), f"Quanto {side}"
    elif spec.product in scripted_payoffs.BUILDERS and p.get("start_date"):
        start = date.fromisoformat(str(p["start_date"]))
        t = scripted_payoffs.terms(
            spec.product, dict(p), start, spec.expiry, spec.currency
        )
        script, title = t.script, spec.product.capitalize()
    else:
        return None
    first = min(date.fromisoformat(x) for x in product_templates._ISO.findall(script))
    return ScriptSpec(
        product="custom",
        title=f"{title} {K:g}" if K else title,
        terms={},
        script=script,
        underlying=spec.underlying,
        currency=spec.currency,
        start=first,
        expiry=spec.expiry,
        closed_form=spec,
    )


__all__ = [
    "BookingError",
    "book",
    "event_dates",
    "for_exposure",
    "from_hand_written",
    "label",
    "mark",
    "season",
]
