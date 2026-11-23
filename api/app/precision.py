"""Price to a precision instead of a path count (issue #331, steps 2 and 3).

The request names an error target ε -- absolute, a share of the price, or
inferred from the market -- and a hard time budget. Then:

1. A pilot run (PILOT_PATHS, price only, its time-step bias estimated) gives
   the per-path dispersion σ and the bias with its own error. The cost of a
   path is measured on runs with the request's own settings, grown until
   their paths outweigh the fixed costs (_path_cost).
2. ε splits between bias and variance as the multilevel engine already
   splits it (Giles): bias <= ε/√2, standard error <= ε/√2, so
   N = (σ / (ε/√2))².
3. The run stops at the first of four rules, always named in the response:
   - bias_floor: the pilot resolved a bias (beyond twice its error) of at
     least ε/√2 -- paths cannot remove it, finer steps or multilevel would;
     the pilot's result is returned;
   - out_of_budget: the pilot predicts the target beyond the time budget;
     the run uses what fits, and says how many paths the target needed;
   - target_reached: sqrt(se² + bias²) <= ε;
   - time_budget: the run used the budget without reaching the target (a
     Sobol run's error is not exactly σ/√N: one doubling is tried if the
     budget allows).

'market' mode (the maintainer's decision of 2026-11-19 on #331): ε = k ×
|vega| × δσ, δσ the vanillas' half bid-ask spread in vol at the product's
horizon (delta_sigma below), so the numerical error stays a share k of what
the market itself knows. The vega is the price's sensitivity to a relative
scale of every vol of the model (the local-vol grid, the SLV leverage, a
flat vol, Heston's v0, theta and xi), by a 1 % bump either way on the
pilot's draws, turned into vol points with the ATM vol at that expiry.

The rule "never trade precision for speed in the main estimator" holds: the
target decides how many paths, never how they are drawn. The greeks follow
the price's path count; they have no target of their own yet.

A time budget makes the path count depend on the machine's speed: a valuation
cut by it (out_of_budget, time_budget) may replay with another count.
"""

from __future__ import annotations

import contextvars
import math
import re
import statistics
import time
from datetime import date
from typing import Callable, Optional, Tuple

from .schemas import PrecisionOutcome, PricingResponse

#: Paths of the pilot run, and of each side of the vega bump.
PILOT_PATHS = 8192
#: The schema's bounds on n_paths.
MIN_PATHS, MAX_PATHS = 1_000, 5_000_000
#: The vega bump: every vol scaled by 1 ± BUMP.
BUMP = 0.01
#: The least time the paths of the cost-measuring run must add over the
#: pilot's before the slope is trusted (timer and scheduling noise).
MIN_SPAN_S = 0.05

#: The relative scale applied to every vol of the model by the pricing in
#: progress (pricing_service reads it): 1.0 outside a vega bump.
VOL_SCALE: contextvars.ContextVar[float] = contextvars.ContextVar(
    "qm_vol_scale", default=1.0
)

_DATE = re.compile(r"^\s*(\d{4}-\d{2}-\d{2})\b", re.M)


class PrecisionUnavailable(ValueError):
    """The target cannot be set (no chain for 'market', a zero price for 'relative')."""


def horizon_years(req, today: date) -> float:
    """The product's life: a library product's maturity, or a script's last
    date from its valuation date."""
    from . import product_templates

    if getattr(req, "product", None):
        if getattr(req, "maturity", None):
            return float(req.maturity)
        return float(product_templates.span_years(req.product))
    start = req.valuation_date or today
    dates = [
        date.fromisoformat(d) for d in _DATE.findall(getattr(req, "script", "") or "")
    ]
    if not dates:
        return 1.0
    return max((max(dates) - start).days / 365.0, 1.0 / 365.0)


def _norm_pdf(x: float) -> float:
    return math.exp(-0.5 * x * x) / math.sqrt(2.0 * math.pi)


def delta_sigma(
    quotes, spot: float, rate: float, dividend: float, horizon: float
) -> Tuple[float, float, float, int]:
    """The vanillas' half-spread in vol at the expiry nearest `horizon`: per
    strike within 10 % of the forward (20 % if fewer than 3), the tighter of
    the call's and the put's (ask - bid) / 2 / vega -- a single side can be
    stale (the calls of SPY's 2026-11-18 snapshot were 15x wider than the
    puts at the same strikes, #331) --, then the median over strikes.
    Returns (delta_sigma, atm_vol, expiry, strikes used)."""
    usable = [
        q
        for q in quotes
        if q.ttm > 0.02
        and q.bid > 0.0
        and q.ask > q.bid
        and q.has_iv
        and q.implied_vol > 0.0
    ]
    if not usable:
        raise PrecisionUnavailable(
            "no two-sided quote with an implied vol in the stored chain"
        )
    expiry = min({q.ttm for q in usable}, key=lambda t: abs(t - horizon))
    fwd = spot * math.exp((rate - dividend) * expiry)
    for band in (0.10, 0.20):
        by_strike: dict = {}
        vols = []
        for q in usable:
            if q.ttm != expiry or abs(math.log(q.strike / fwd)) > band:
                continue
            sig, t = q.implied_vol, q.ttm
            d1 = (math.log(fwd / q.strike) + 0.5 * sig * sig * t) / (sig * math.sqrt(t))
            vega = spot * math.exp(-dividend * t) * _norm_pdf(d1) * math.sqrt(t)
            if vega <= 0.0:
                continue
            half = 0.5 * (q.ask - q.bid) / vega
            by_strike[q.strike] = min(half, by_strike.get(q.strike, math.inf))
            vols.append(sig)
        if len(by_strike) >= 3:
            return (
                statistics.median(by_strike.values()),
                statistics.median(vols),
                expiry,
                len(by_strike),
            )
    raise PrecisionUnavailable(
        f"fewer than 3 near-the-money strikes with two-sided quotes at the {expiry:.2f}y expiry"
    )


def _with_paths(req, n: int, **over):
    return req.model_copy(update={"n_paths": int(n), "precision": None, **over})


def _resolved_bias(r: PricingResponse, sigmas: float = 2.0) -> float:
    """The bias when the estimate resolves it (beyond `sigmas` of its own
    error), else 0. The pilot asks for 3: its error comes from 16 Sobol
    replicates, itself uncertain."""
    b = r.discretisation_bias
    if b is None or b <= 0.0:
        return 0.0
    err = r.discretisation_bias_std_error
    return b if err is None or b > sigmas * err else 0.0


def _timed(price, req) -> Tuple[PricingResponse, float]:
    t = time.perf_counter()
    r = price(req)
    return r, time.perf_counter() - t


def _path_cost(price, run_req, needed: int, deadline: float) -> Tuple[float, float]:
    """(fixed, per path) seconds of the run asked for, measured on runs with
    its own settings (its greeks, its bias estimate): a fixed factor over a
    price-only run under-stated an adjoint one by far, with 12,903
    sensitivities under SLV (#347).

    A pricing's fixed costs -- market data, Python, a kernel launch -- dwarf
    a GPU path's at the pilot's size: two runs of 8,192 and 32,768 paths both
    take ~0.04 s on the V100s, their difference is noise, and a budget
    derived from it capped nothing (#347). So the second run grows fourfold
    until its paths add at least
    three times the fixed part (and MIN_SPAN_S) -- that part's jitter then
    moves the slope by about a third at most -- and the slope is measured.
    It stops growing once it holds the paths needed, or when the next one
    would take more than half of what is left of the budget: the cost per
    path is then bounded above by the whole run's time over its paths, which
    over-states it and so keeps the final run within the budget."""
    _, t1 = _timed(price, run_req)
    n = 4 * PILOT_PATHS
    while True:
        _, tn = _timed(price, run_req.model_copy(update={"n_paths": n}))
        span = tn - t1
        per_path = span / (n - PILOT_PATHS)
        fixed = t1 - per_path * PILOT_PATHS
        if span >= max(3.0 * fixed, MIN_SPAN_S):
            return max(fixed, 0.0), per_path
        left = deadline - time.perf_counter()
        if n >= min(needed, MAX_PATHS) or 4 * tn > 0.5 * left:
            per_path = max(tn / n, 1e-12)
            return max(t1 - per_path * PILOT_PATHS, 0.0), per_path
        n = min(4 * n, MAX_PATHS)


def price_to_precision(
    req,
    price: Callable[[object], PricingResponse],
    market_quotes: Optional[Callable[[], tuple]] = None,
) -> PricingResponse:
    """`price(req)` priced to `req.precision` (see the module's docstring).
    `market_quotes()` -> (quotes, spot, rate, dividend, valuation date) for
    'market' mode."""
    target_spec = req.precision
    t0 = time.perf_counter()
    budget = target_spec.time_budget_s
    outcome = dict(mode=target_spec.mode, pilot_paths=PILOT_PATHS)
    plain = dict(greeks_method="none", second_order=False, bias_estimate=False)

    # The pilot: dispersion and bias. The cost of a path is measured once the
    # path count needed is known (_path_cost).
    pilot = price(
        _with_paths(
            req,
            PILOT_PATHS,
            greeks_method="none",
            second_order=False,
            bias_estimate=True,
        )
    )

    if target_spec.mode == "absolute":
        eps = target_spec.value
    elif target_spec.mode == "relative":
        if pilot.npv == 0.0:
            raise PrecisionUnavailable("a relative target needs a non-zero price")
        eps = target_spec.value * abs(pilot.npv)
    else:
        if market_quotes is None:
            raise PrecisionUnavailable(
                "precision mode 'market' needs one underlying with a stored option chain"
            )
        quotes, spot, rate, dividend, valuation = market_quotes()
        horizon = horizon_years(req, valuation)
        ds, atm, expiry, _ = delta_sigma(quotes, spot, rate, dividend, horizon)
        bumped = []
        for s in (1.0 + BUMP, 1.0 - BUMP):
            token = VOL_SCALE.set(s)
            try:
                bumped.append(price(_with_paths(req, 4 * PILOT_PATHS, **plain)).npv)
            finally:
                VOL_SCALE.reset(token)
        vega_scale = (bumped[0] - bumped[1]) / (2.0 * BUMP)
        eps = target_spec.value * abs(vega_scale) * ds / atm
        if not eps > 0.0:
            raise PrecisionUnavailable(
                "the product shows no vega: set an absolute or relative target"
            )
        outcome.update(
            delta_sigma=ds, vega_scale=vega_scale, atm_vol=atm, expiry=expiry
        )
    outcome["target"] = eps
    half = eps / math.sqrt(2.0)

    bias = _resolved_bias(pilot, 3.0)
    if bias >= half:
        return _done(
            pilot,
            outcome,
            "bias_floor",
            PILOT_PATHS,
            None,
            bias,
            t0,
            f"the time-step bias {bias:.4g} alone is at least target/sqrt(2) = {half:.4g}",
        )

    sigma = pilot.mc_std_error * math.sqrt(PILOT_PATHS)
    needed = (
        max(MIN_PATHS, math.ceil((sigma / half) ** 2)) if sigma > 0.0 else MIN_PATHS
    )
    fixed, per_path = _path_cost(
        price, _with_paths(req, PILOT_PATHS), needed, t0 + budget
    )
    remaining = budget - (time.perf_counter() - t0)
    fits = int(max(MIN_PATHS, min(MAX_PATHS, (remaining - fixed) / per_path)))
    n = min(needed, MAX_PATHS)
    stopped = "out_of_budget" if needed > MAX_PATHS else None
    if n > fits:
        n, stopped = fits, "out_of_budget"
    final = price(_with_paths(req, n))
    err = math.hypot(final.mc_std_error, _resolved_bias(final))
    # More paths resolve a bias the pilot could not: if it alone is the
    # floor, say so rather than blame the budget.
    if err > eps and _resolved_bias(final) >= half:
        return _done(
            final,
            outcome,
            "bias_floor",
            n,
            needed,
            _resolved_bias(final),
            t0,
            f"the time-step bias {_resolved_bias(final):.4g}, resolved at {n} paths, is at least "
            f"target/sqrt(2) = {half:.4g}: finer steps or multilevel would lower it, not paths",
        )
    if stopped is None and err > eps:
        # Sobol: the error need not fall as 1/sqrt(N); one doubling if it fits.
        remaining = budget - (time.perf_counter() - t0)
        if 2 * n <= MAX_PATHS and fixed + per_path * 2 * n < remaining:
            n *= 2
            final = price(_with_paths(req, n))
            err = math.hypot(final.mc_std_error, _resolved_bias(final))
        if err > eps:
            stopped = "time_budget"
    return _done(
        final,
        outcome,
        stopped or "target_reached",
        n,
        needed,
        _resolved_bias(final),
        t0,
    )


def _done(
    r: PricingResponse,
    outcome: dict,
    stopped: str,
    n: int,
    needed,
    bias: float,
    t0: float,
    note: str = "",
) -> PricingResponse:
    p = PrecisionOutcome(
        stopped=stopped,
        paths=n,
        paths_needed=needed,
        achieved_error=math.hypot(r.mc_std_error, bias),
        seconds=time.perf_counter() - t0,
        note=note,
        **outcome,
    )
    return r.model_copy(
        update={
            "precision": p,
            "diagnostics": r.diagnostics
            + f" | priced to a precision: {stopped} ({n} paths, target {p.target:.4g})",
        }
    )
