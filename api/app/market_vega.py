"""Vega by quoted option: the sensitivity of a product's local-vol price to
each implied vol of the stored option chain -- the Dupire superbucket
(blueprint/wp/17-aad.md §11, lot 17h).

1. The product is priced under the local vol calibrated from the stored chain
   with adjoint differentiation: dV/dsigma_loc on every point of the Dupire
   grid, in one pass (lot 17e).
2. market/superbucket.hpp takes those model risks to the market: through the
   Dupire formula on an AAD tape, then through each SVI fit by the implicit
   function theorem -- dV/d(each quoted implied vol).

The Monte-Carlo error of a quote's vega is measured, not guessed: the AAD
runs on BATCHES independent seeds, each batch goes through the superbucket
(it is linear in dV/dsigma_loc), and the error is the dispersion of the
batch vegas. The batches run on parallel threads (the GIL is released).

Every vega is per vol point: the price change for a +1 % move of that one
quote, the rest of the chain unchanged.
"""

from __future__ import annotations

import math
import re
from concurrent.futures import ThreadPoolExecutor
from typing import Dict, List

import quantmodeling as qm

from . import market_snapshot, product_templates
from .schemas import (
    MaturityVega,
    MarketVegaResponse,
    QuoteVegaRow,
    ScriptedProductRequest,
)

BATCHES = 8
WORKERS = 8
_LVOL = re.compile(r"^lvol\[(\d+),(\d+)\]$")


def _dV_dsigma(risks: List[Dict], n_maturities: int, size: int) -> List[float]:
    out = [0.0] * size
    for r in risks:
        m = _LVOL.match(r["label"])
        if m:
            out[int(m.group(1)) * n_maturities + int(m.group(2))] = float(r["value"])
    return out


def _mean_se(xs: List[float]):
    n = len(xs)
    mean = sum(xs) / n
    var = sum((x - mean) ** 2 for x in xs) / (n - 1) if n > 1 else 0.0
    return mean, math.sqrt(var / n)


def market_vega(req: ScriptedProductRequest) -> MarketVegaResponse:
    t = product_templates.get(req.product)
    if t.underlyings != 1:
        raise ValueError(
            "vega by quoted option needs one underlying: its local vol is "
            "calibrated from that underlying's option chain"
        )
    if not req.ticker:
        raise ValueError("vega by quoted option needs a ticker (its stored chain)")
    m = market_snapshot.local_vol_market(req.ticker, req.rate, req.valuation_date)
    script = product_templates.render(req.product, req.terms, m.valuation_date)
    size = len(m.K_grid) * len(m.T_grid)
    per_batch = max(req.n_paths // BATCHES, 1000)

    def run(b: int):
        priced = qm.price_script(
            script,
            m.spot,
            req.rate,
            m.dividend,
            0.0,
            m.valuation_date.isoformat(),
            n_paths=per_batch,
            seed=req.seed + b,
            greeks_method="aad",
            model="local_vol",
            K_grid=m.K_grid,
            T_grid=m.T_grid,
            sigma_loc_flat=m.sigma_loc_flat,
            steps_per_year=req.steps_per_year,
        )
        bucket = qm.dupire_superbucket(
            m.svi_slices,
            m.spot,
            req.rate,
            m.dividend,
            m.k_min,
            m.k_max,
            m.n_strikes,
            m.n_maturities,
            _dV_dsigma(priced["risks"], len(m.T_grid), size),
        )
        return priced["npv"], bucket["quotes"]

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        batches = list(pool.map(run, range(BATCHES)))

    npv, npv_se = _mean_se([b[0] for b in batches])
    template = batches[0][1]
    rows: List[QuoteVegaRow] = []
    for i, q in enumerate(template):
        v, se = _mean_se([b[1][i]["vega"] * 0.01 for b in batches])
        rows.append(
            QuoteVegaRow(
                ttm=q["ttm"],
                strike=q["strike"],
                log_moneyness=q["log_moneyness"],
                implied_vol=q["implied_vol"],
                vega=v,
                std_error=se,
            )
        )
    by_ttm: Dict[float, List[int]] = {}
    for i, r in enumerate(rows):
        by_ttm.setdefault(r.ttm, []).append(i)
    maturities = []
    for ttm, idx in sorted(by_ttm.items()):
        v, se = _mean_se([sum(b[1][i]["vega"] * 0.01 for i in idx) for b in batches])
        maturities.append(
            MaturityVega(ttm=ttm, vega=v, std_error=se, n_quotes=len(idx))
        )
    total, total_se = _mean_se([sum(q["vega"] * 0.01 for q in b[1]) for b in batches])
    return MarketVegaResponse(
        product=req.product,
        ticker=m.ticker,
        snapshot=m.valuation_date,
        npv=npv,
        mc_std_error=npv_se,
        total_vega=total,
        total_std_error=total_se,
        maturities=maturities,
        quotes=rows,
        method=(
            f"Local vol calibrated from the stored chain; adjoint Monte-Carlo on "
            f"{BATCHES} batches of {per_batch:,} paths; through Dupire on a tape, "
            "then each SVI fit by the implicit function theorem (Gauss-Newton)."
        ),
    )


__all__ = ["BATCHES", "market_vega"]
