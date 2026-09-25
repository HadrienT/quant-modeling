"""Long or short what? The risk profile of a library product, the way
Bouzoubaa & Osseiran analyse each structure: for every market parameter, does
the holder gain or lose when it rises.

Computed, not asserted: the product is priced on a stated reference market,
each parameter is bumped, and the sign of the price change is the position.
Levels are fixed at the reference spot on the trade date (the strike date is
passed as a past fixing), so a spot bump moves the underlying against fixed
strikes and barriers, as a desk's delta does.

Standard errors are those of the *paired* differences: every scenario is
priced on BATCHES independent seeds with the same seeds for base and bump
(common random numbers), and the error of a change is the dispersion of its
BATCHES paired differences. A change within two such errors is reported as
not significant, never as a sign.

Parameters: spot (delta), spot convexity (gamma), volatility, rates,
dividends, correlation (several underlyings), and two smile factors on one
underlying -- skew (a local-vol surface whose vol falls with the strike,
against a flat one under the same engine) and vol of vol (Heston's xi, with
v0 = theta = the reference variance and no spot/vol correlation).
"""

from __future__ import annotations

import math
import re
from datetime import date, timedelta
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from functools import lru_cache
from typing import Dict, List, Optional, Tuple

import quantmodeling as qm

from . import product_templates

REFERENCE = dict(spot=100.0, vol=0.25, rate=0.03, dividend=0.01, correlation=0.5)
BATCHES = 8
PATHS_PER_BATCH = 5_000
WORKERS = 8
SKEW = -0.10  # d sigma_loc / d ln(K / S0) of the skewed surface
XI_LOW, XI_HIGH = 0.2, 0.6
#: A change smaller than this fraction of the price is reported as negligible.
MATERIALITY = 1e-4


@dataclass(frozen=True)
class Scenario:
    spot: float = 1.0  # multiple of the reference spot today
    vol: float = 0.0  # added to the reference vol
    rate: float = 0.0
    dividend: float = 0.0
    correlation: float = 0.0
    model: str = "black_scholes"
    skew: float = 0.0  # local_vol only
    xi: float = 0.0  # heston only


@dataclass(frozen=True)
class Row:
    factor: str
    bump: str
    change: float
    std_error: float
    position: str  # "long" | "short" | "not significant" | "negligible"


def _grid(T_max: float, skew: float, vol: float):
    K = [100.0 * math.exp(x / 10.0) for x in range(-20, 21)]
    T = [0.02, 0.25, 0.5, 1.0, 2.0, max(T_max, 2.0) + 1.0]
    sigma = [max(vol + skew * math.log(k / 100.0), 0.05) for k in K for _ in T]
    return K, T, sigma


def _price(
    script: str, n: int, first: str, horizon: float, sc: Scenario, seed: int
) -> float:
    ref = REFERENCE
    spot = ref["spot"] * sc.spot
    vol = ref["vol"] + sc.vol
    rate, div = ref["rate"] + sc.rate, ref["dividend"] + sc.dividend
    kw: Dict = dict(
        n_paths=PATHS_PER_BATCH,
        seed=seed,
        historical_fixings={first: [ref["spot"]] * n},
    )
    if n > 1:
        rho = min(ref["correlation"] + sc.correlation, 0.999)
        kw.update(
            spots=[spot] * n,
            dividends=[div] * n,
            vols=[vol] * n,
            correlation=[1.0 if i == j else rho for i in range(n) for j in range(n)],
        )
    elif sc.model == "local_vol":
        K, T, sigma = _grid(horizon, sc.skew, vol)
        kw.update(model="local_vol", K_grid=K, T_grid=T, sigma_loc_flat=sigma)
    elif sc.model == "heston":
        kw.update(
            model="heston",
            heston=dict(v0=vol * vol, kappa=2.0, theta=vol * vol, xi=sc.xi, rho=0.0),
        )
    return qm.price_script(script, spot, rate, div, vol, first, **kw)["npv"]


def _paired(values: Dict[Scenario, List[float]], combo: List[Tuple[float, Scenario]]):
    """Mean and paired standard error of sum(w * V(scenario)) over batches."""
    diffs = [sum(w * values[sc][b] for w, sc in combo) for b in range(BATCHES)]
    mean = sum(diffs) / BATCHES
    var = sum((d - mean) ** 2 for d in diffs) / (BATCHES - 1)
    return mean, math.sqrt(var / BATCHES)


def _row(factor: str, bump: str, mean: float, se: float, price: float) -> Row:
    if abs(mean) <= 2.0 * se:
        position = "not significant"
    elif abs(mean) < MATERIALITY * abs(price):
        position = "negligible"
    else:
        position = "long" if mean > 0 else "short"
    return Row(factor, bump, mean, se, position)


@lru_cache(maxsize=64)
def _profile(slug: str, terms: Tuple[Tuple[str, float], ...]):
    t = product_templates.get(slug)
    script = product_templates.render(slug, dict(terms), _first_date(t.script))
    first_day = _first_date(script)
    first = first_day.isoformat()
    # Read the horizon the day before: on the strike date itself the first
    # event is a past fixing, which validate_script has no value for.
    parsed = qm.validate_script(script, (first_day - timedelta(days=1)).isoformat())
    horizon = parsed["events"][-1]["t"] if parsed["events"] else 1.0
    n = t.underlyings

    base = Scenario()
    rows_spec: List[Tuple[str, str, List[Tuple[float, Scenario]]]] = [
        (
            "Spot (delta)",
            "+/-1% of spot",
            [(0.5, Scenario(spot=1.01)), (-0.5, Scenario(spot=0.99))],
        ),
        (
            "Spot convexity (gamma)",
            "+/-1% of spot",
            [(1, Scenario(spot=1.01)), (1, Scenario(spot=0.99)), (-2, base)],
        ),
        ("Volatility (vega)", "+1 vol point", [(1, Scenario(vol=0.01)), (-1, base)]),
        ("Interest rates (rho)", "+50 bp", [(1, Scenario(rate=0.005)), (-1, base)]),
        ("Dividends", "+50 bp of yield", [(1, Scenario(dividend=0.005)), (-1, base)]),
    ]
    if n > 1:
        rows_spec.append(
            (
                "Correlation",
                "+10 points between every pair",
                [(1, Scenario(correlation=0.10)), (-1, base)],
            )
        )
    else:
        lv0, lvs = Scenario(model="local_vol"), Scenario(model="local_vol", skew=SKEW)
        hl, hh = Scenario(model="heston", xi=XI_LOW), Scenario(
            model="heston", xi=XI_HIGH
        )
        rows_spec += [
            (
                "Skew",
                "local vol falling 10 points per unit of log-strike",
                [(1, lvs), (-1, lv0)],
            ),
            ("Vol of vol", f"Heston xi {XI_LOW} -> {XI_HIGH}", [(1, hh), (-1, hl)]),
        ]

    scenarios = sorted(
        {sc for _, _, combo in rows_spec for _, sc in combo} | {base}, key=repr
    )
    jobs = [(sc, b) for sc in scenarios for b in range(BATCHES)]
    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        prices = list(
            pool.map(lambda j: _price(script, n, first, horizon, j[0], j[1] + 1), jobs)
        )
    values: Dict[Scenario, List[float]] = {sc: [0.0] * BATCHES for sc in scenarios}
    for (sc, b), v in zip(jobs, prices):
        values[sc][b] = v

    price, price_se = _paired(values, [(1, base)])
    rows = [
        _row(f, bump, *_paired(values, combo), price) for f, bump, combo in rows_spec
    ]
    return price, price_se, rows


def _first_date(script: str) -> date:
    return date.fromisoformat(re.search(r"\d{4}-\d{2}-\d{2}", script)[0])


def risk_profile(slug: str, terms: Optional[Dict[str, float]] = None):
    return _profile(slug, tuple(sorted((terms or {}).items())))


__all__ = ["BATCHES", "PATHS_PER_BATCH", "REFERENCE", "Row", "risk_profile"]
