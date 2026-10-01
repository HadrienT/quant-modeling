"""Rates derivatives — the /rates page (blueprint/wp/21-rates.md).

From a set of quotes, in one request: the OIS discount curve, the projection
curve of a floating index bootstrapped on it (multi-curve), a vanilla swap on
the two curves, a Hull-White model calibrated to a grid of ATM swaption normal
vols, and one swaption priced under Bachelier, shifted Black, shifted SABR and
Hull-White — European and Bermudan on the same swap.

**Two sets of quotes.** The page starts from an illustrative EUR set (€STR
OIS, EURIBOR 6M) flagged as such: no free source publishes dealer quotes of
OIS swap rates, an IBOR swap curve or swaption vols. For USD SOFR there is the
next best thing, and it is real: the swaps and swaptions actually traded,
published by DTCC and stored by `data-ingest` (`market_request`,
`swaption_market.py`). Either way every number is computed from the quotes
that are sent, which the user can edit.
The C++ does the work (`qm.bootstrap_ois_curve`, `bootstrap_projection_curve`,
`price_swap`, `calibrate_hull_white`, `price_swaption`); this module shapes it
and carries the methodology the page shows.
"""

from __future__ import annotations

import math
from datetime import date, timedelta
from typing import Dict, List, Optional, Tuple

import quantmodeling as qm

from . import db, swaption_market
from .rates_derivatives_schemas import (
    CalibrationPoint,
    CurvePoint,
    CurvesResult,
    HullWhiteCalibrationResult,
    RatesAnalysisRequest,
    RatesAnalysisResponse,
    SwapPeriod,
    SwapResult,
    SwaptionModelPrice,
    SwaptionResult,
)
from .schemas import MethodologySection

#: Illustrative EUR quotes: the €STR OIS curve and the EURIBOR 6M curve of a
#: plausible market (rates as decimals, vols in rate units). NOT market data.
EXAMPLE = {
    "currency": "EUR",
    "label": (
        "Illustrative EUR quotes (€STR OIS, EURIBOR 6M, ATM swaption normal vols)"
        " — not market data: no free source publishes them. Replace them with"
        " your own."
    ),
    "deposits": [(0.25, 0.0192), (0.5, 0.0194)],
    "ois": [
        (1, 0.0197),
        (2, 0.0203),
        (3, 0.0211),
        (5, 0.0226),
        (7, 0.0240),
        (10, 0.0256),
        (15, 0.0270),
        (20, 0.0272),
        (30, 0.0262),
    ],
    "fras": [(0.0, 0.5, 0.0213), (0.5, 1.0, 0.0212)],
    "swaps": [
        (2, 0.0224),
        (3, 0.0231),
        (5, 0.0245),
        (7, 0.0258),
        (10, 0.0273),
        (15, 0.0287),
        (20, 0.0289),
        (30, 0.0279),
    ],
    "swaption_vols": [
        (1, 2, 0.0072),
        (1, 5, 0.0074),
        (1, 10, 0.0073),
        (2, 5, 0.0075),
        (2, 10, 0.0073),
        (5, 5, 0.0072),
        (5, 10, 0.0069),
        (10, 10, 0.0063),
    ],
}

#: Tenor grid the curves are drawn on (years).
_GRID = [0.25 * i for i in range(1, 121)]


def _zero(df: float, t: float) -> float:
    return -math.log(df) / t


def _curves(req: RatesAnalysisRequest) -> Tuple[dict, dict, CurvesResult]:
    ois = qm.bootstrap_ois_curve(
        [(q.tenor, q.rate) for q in req.deposits], [(q.tenor, q.rate) for q in req.ois]
    )
    proj = qm.bootstrap_projection_curve(
        ois["times"],
        ois["discount_factors"],
        [(f.start, f.end, f.rate) for f in req.fras],
        [(q.tenor, q.rate) for q in req.swaps],
        req.fixed_frequency,
        req.float_frequency,
    )
    last = max(ois["times"][-1], proj["times"][-1])
    grid = [t for t in _GRID if t <= last + 1e-9]
    period = 1.0 / req.float_frequency
    d_ois = qm.rate_curve_discount_factors(
        ois["times"], ois["discount_factors"], grid + [t + period for t in grid]
    )
    d_proj = qm.rate_curve_discount_factors(
        proj["times"], proj["discount_factors"], grid + [t + period for t in grid]
    )
    n = len(grid)
    points = []
    for i, t in enumerate(grid):
        # Forwards over the index period starting at t: simple rates.
        f_ois = (d_ois[i] / d_ois[n + i] - 1.0) / period
        f_idx = (d_proj[i] / d_proj[n + i] - 1.0) / period
        points.append(
            CurvePoint(
                tenor=t,
                ois_zero=_zero(d_ois[i], t),
                ois_forward=f_ois,
                index_forward=f_idx,
                basis_bp=(f_idx - f_ois) * 1e4,
            )
        )
    # Repricing check: each input swap at its own quote on the built curves.
    worst = 0.0
    for q in req.swaps:
        v = qm.price_swap(
            ois["times"],
            ois["discount_factors"],
            proj["times"],
            proj["discount_factors"],
            0.0,
            q.tenor,
            q.rate,
            req.fixed_frequency,
            req.float_frequency,
        )
        worst = max(worst, abs(v["par_rate"] - q.rate) * 1e4)
    return (
        ois,
        proj,
        CurvesResult(
            ois_pillars=ois["times"],
            index_pillars=proj["times"],
            points=points,
            max_repricing_error_bp=worst,
        ),
    )


def _swap(req: RatesAnalysisRequest, ois: dict, proj: dict) -> SwapResult:
    s = req.swap
    v = qm.price_swap(
        ois["times"],
        ois["discount_factors"],
        proj["times"],
        proj["discount_factors"],
        s.start,
        s.tenor,
        s.fixed_rate,
        req.fixed_frequency,
        req.float_frequency,
        s.notional,
        s.payer,
        0.0,
    )
    return SwapResult(
        npv=v["npv"],
        par_rate=v["par_rate"],
        annuity=v["annuity"],
        pv01=v["pv01"],
        fixed_leg=v["fixed_leg"],
        floating_leg=v["floating_leg"],
        fixed_periods=[
            SwapPeriod(start=a, end=b, payment=c, accrual=d, discount=e, forward=None)
            for a, b, c, d, e in v["fixed_periods"]
        ],
        floating_periods=[
            SwapPeriod(start=a, end=b, payment=c, accrual=d, discount=e, forward=f)
            for a, b, c, d, e, f in v["floating_periods"]
        ],
    )


def _calibration(
    req: RatesAnalysisRequest, ois: dict, proj: dict
) -> HullWhiteCalibrationResult:
    c = qm.calibrate_hull_white(
        ois["times"],
        ois["discount_factors"],
        proj["times"],
        proj["discount_factors"],
        [(q.expiry, q.tenor, q.normal_vol) for q in req.swaption_vols],
        req.fixed_frequency,
        req.float_frequency,
        req.hull_white_mean_reversion,
    )
    return HullWhiteCalibrationResult(
        mean_reversion=c["mean_reversion"],
        sigma=c["sigma"],
        mean_reversion_fixed=req.hull_white_mean_reversion is not None,
        rmse_bp=c["rmse_bp"],
        worst_bp=c["worst_bp"],
        iterations=c["iterations"],
        converged=c["converged"],
        seconds=c["seconds"],
        points=[
            CalibrationPoint(
                expiry=q.expiry,
                tenor=q.tenor,
                strike=k,
                market_vol=mv,
                model_vol=(
                    None if model is None or not math.isfinite(model) else model
                ),
            )
            for q, k, mv, model in zip(
                req.swaption_vols, c["strikes"], c["market_vols"], c["model_vols"]
            )
        ],
    )


def _nearest_vol(req: RatesAnalysisRequest) -> Optional[float]:
    """The quoted ATM normal vol closest to the swaption's (expiry, tenor):
    the Bachelier price's vol when the request gives none."""
    sw = req.swaption
    if not req.swaption_vols:
        return None
    best = min(
        req.swaption_vols,
        key=lambda q: (q.expiry - sw.expiry) ** 2 + (q.tenor - sw.tenor) ** 2,
    )
    return best.normal_vol


def _swaption(
    req: RatesAnalysisRequest,
    ois: dict,
    proj: dict,
    hw: HullWhiteCalibrationResult,
) -> SwaptionResult:
    sw = req.swaption
    normal_vol = sw.normal_vol if sw.normal_vol is not None else _nearest_vol(req)
    exercises = [sw.expiry + i for i in range(int(round(sw.tenor)))]
    out = qm.price_swaption(
        ois["times"],
        ois["discount_factors"],
        proj["times"],
        proj["discount_factors"],
        sw.expiry,
        sw.tenor,
        sw.strike,
        sw.payer,
        req.fixed_frequency,
        req.float_frequency,
        sw.notional,
        normal_vol=normal_vol,
        lognormal_vol=sw.lognormal_vol,
        shift=sw.shift,
        sabr=(
            (sw.sabr.alpha, sw.sabr.beta, sw.sabr.rho, sw.sabr.nu) if sw.sabr else None
        ),
        hull_white=(hw.mean_reversion, hw.sigma),
        bermudan_exercises=exercises,
    )
    forward, annuity, strike = out["forward"], out["annuity"], out["strike"]
    prices: List[SwaptionModelPrice] = []

    def add(model: str, key: str, detail: str) -> None:
        if key not in out:
            return
        price = out[key]
        # The C++ prices on annuity × notional: invert on the same.
        implied = qm.bachelier_implied_vol(
            sw.payer, price, forward, strike, sw.expiry, annuity * sw.notional
        )
        prices.append(
            SwaptionModelPrice(
                model=model,
                price=price,
                implied_normal_vol=implied if math.isfinite(implied) else None,
                detail=detail,
            )
        )

    add(
        "Bachelier (normal)",
        "bachelier",
        f"σ_N = {normal_vol * 1e4:.1f}bp"
        + (" (nearest quoted ATM vol)" if sw.normal_vol is None else ""),
    )
    if sw.lognormal_vol is not None:
        add(
            "Black (shifted lognormal)",
            "black",
            f"σ = {sw.lognormal_vol:.1%}, shift {sw.shift:.2%}",
        )
    if sw.sabr is not None:
        add(
            "SABR (shifted, Hagan 2002)",
            "sabr",
            f"implied lognormal vol {out['sabr_lognormal_vol']:.1%}",
        )
    add(
        "Hull-White (calibrated)",
        "hull_white",
        f"a = {hw.mean_reversion:.4f}, σ = {hw.sigma * 1e4:.1f}bp",
    )
    bermudan = out.get("hull_white_bermudan")
    european_hw = out.get("hull_white")
    return SwaptionResult(
        forward=forward,
        annuity=annuity,
        strike=strike,
        prices=prices,
        bermudan_price=bermudan,
        bermudan_exercises=exercises,
        switch_premium=(
            None if bermudan is None or european_hw is None else bermudan - european_hw
        ),
    )


def analyse(req: RatesAnalysisRequest) -> RatesAnalysisResponse:
    ois, proj, curves = _curves(req)
    hw = _calibration(req, ois, proj)
    return RatesAnalysisResponse(
        curves=curves,
        swap=_swap(req, ois, proj),
        hull_white=hw,
        swaption=_swaption(req, ois, proj, hw),
        methodology=methodology(),
    )


def methodology() -> List[MethodologySection]:
    sections: List[Tuple[str, List[str]]] = [
        (
            "Where the quotes come from",
            [
                "No free source publishes dealer quotes of OIS swap rates, the swap "
                "curve of a floating index or swaption volatilities. The page starts "
                "from illustrative EUR quotes, which are not market data, and prices "
                "whatever quotes you enter. The government curves of the Market page "
                "are bond yields: a different curve, not an input here.",
                "The USD SOFR set is market data of another kind: not quotes but "
                "trades. US swap dealers must report every swap to a repository, which "
                "publishes its price and size (CFTC public dissemination; DTCC's "
                "repository, stored daily). The swap curve is, per tenor, the median "
                "fixed rate of the spot-starting swaps traded on the latest day. Each "
                "swaption vol is the median, over the last days, of the normal vols "
                "implied by the traded premiums: premium / notional = annuity × "
                "Bachelier(forward, strike, expiry, σ), solved for σ on the swap curve "
                "of the trade's day. The number of trades behind each point is shown.",
                "What is left out, and why: capped notionals (the premium is published "
                "in full, the notional is not); novations and amendments (not prices); "
                "trades away from the money (the files do not say whether a call is a "
                "payer or a receiver, and only at the money does it not matter); "
                "expiries beyond two years (their premium appears to be paid at expiry "
                "rather than up front, which the files do not say). A straddle traded "
                "on a platform carries the premium of both legs on each leg and counts "
                "for half. These are medians of a handful of trades, not a dealer's "
                "surface: read the trade counts.",
            ],
        ),
        (
            "Two curves, not one",
            [
                "A collateralised swap is discounted at the collateral rate, the "
                "overnight rate (€STR, SOFR): the OIS curve. Since 2008 a 6-month "
                "index fixes above the compounded overnight rate by a basis that one "
                "curve cannot hold, so the index gets its own projection curve, "
                "bootstrapped from FRAs and par swaps priced with OIS discounting "
                "(Ametrano & Bianchetti, 2013).",
                "The OIS curve bootstraps like par bonds: the compounded overnight leg "
                "of a spot OIS is worth 1 − P(T) on its own curve. Each projection "
                "pillar is then solved so that its quote reprices exactly, with every "
                "earlier pillar fixed; the maximum repricing error of the input swaps "
                "is shown with the curves. Discount factors are interpolated "
                "log-linearly (piecewise-flat forwards, always positive discount "
                "factors) and the forward is held flat outside the pillars.",
            ],
        ),
        (
            "Swap",
            [
                "Fixed leg: rate × accrual × notional, discounted on OIS. Floating "
                "leg: the index forward over each period, from the projection curve, "
                "discounted on OIS. The par rate is the fixed rate worth zero; the "
                "PV01 is the annuity × notional × 1bp. Accruals are period lengths "
                "in years on a regular schedule (no calendar or day-count "
                "adjustment).",
            ],
        ),
        (
            "Swaption models",
            [
                "Bachelier: the forward swap rate is Gaussian, the quote convention "
                "of swaption desks since rates went to zero and below. Black: the "
                "rate plus a shift is lognormal. SABR: Hagan et al. (2002)'s "
                "implied-vol expansion on the shifted rate. All three price on the "
                "same multi-curve forward and annuity.",
                "Hull-White: dr = (θ(t) − a r)dt + σ dW, fitted exactly to the OIS "
                "curve, the index curve kept at a deterministic spread (Andersen & "
                "Piterbarg 2010, ch. 10). The European price integrates the exercise "
                "value in closed form over the region where the swap is worth "
                "something (Jamshidian's decomposition generalised to multi-curve "
                "cash flows). (a, σ) are calibrated by Levenberg-Marquardt to the ATM "
                "normal vols above; the residuals are in bp of normal vol. A "
                "one-factor, two-parameter model cannot match a whole grid: the fit "
                "error is the model's, not noise.",
                "Bermudan: the right to enter the swap on each annual date from the "
                "expiry, priced by backward induction on a grid of the Hull-White "
                "state under the terminal measure, with exact Gaussian transitions "
                "between exercise dates. The switch premium is the Bermudan minus the "
                "European on the same swap.",
            ],
        ),
    ]
    return [MethodologySection(title=t, paragraphs=p) for t, p in sections]


def example_request() -> Dict:
    """The page's starting point, the illustrative quotes above."""
    e = EXAMPLE
    return {
        "currency": e["currency"],
        "label": e["label"],
        "fixed_frequency": 1,
        "float_frequency": 2,
        "deposits": [{"tenor": t, "rate": r} for t, r in e["deposits"]],
        "ois": [{"tenor": t, "rate": r} for t, r in e["ois"]],
        "fras": [{"start": s, "end": en, "rate": r} for s, en, r in e["fras"]],
        "swaps": [{"tenor": t, "rate": r} for t, r in e["swaps"]],
        "swaption_vols": [
            {"expiry": x, "tenor": t, "normal_vol": v} for x, t, v in e["swaption_vols"]
        ],
        "hull_white_mean_reversion": None,
        "swap": {
            "start": 0.0,
            "tenor": 10.0,
            "fixed_rate": 0.0273,
            "notional": 10_000_000.0,
            "payer": True,
        },
        "swaption": {
            "expiry": 2.0,
            "tenor": 10.0,
            "strike": None,
            "payer": True,
            "notional": 10_000_000.0,
            "normal_vol": None,
            "lognormal_vol": 0.185,
            "shift": 0.01,
            "sabr": {"alpha": 0.037, "beta": 0.5, "rho": -0.2, "nu": 0.3},
        },
    }


# ── USD SOFR quotes from traded swaps and swaptions ──────────────────────────

#: What the DTCC sources of data-ingest call a SOFR swap.
SOFR_SWAP = "NA/Swap OIS USD"
#: Days of trades behind the swaption vols.
MARKET_WINDOW_DAYS = 10
#: Older than this, the latest swap curve is not "the market" any more.
MARKET_STALE_AFTER_DAYS = 7
#: Fewer vol points than this cannot calibrate (a, σ).
_MIN_VOL_POINTS = 3


class RatesMarketUnavailable(RuntimeError):
    """Market quotes that cannot be built from what is in the store."""


def market_request(today: Optional[date] = None) -> Dict:
    """USD SOFR quotes built from the trades in the store, in the shape of
    `example_request`, with what each number rests on.

    No fallback: a missing or stale curve, or too few swaption trades, is an
    explicit RatesMarketUnavailable.
    """
    today = today or date.today()
    curves = db.dtcc_swap_curves(
        SOFR_SWAP, today - timedelta(days=MARKET_WINDOW_DAYS + MARKET_STALE_AFTER_DAYS)
    )
    complete = {d: rates for d, rates in curves.items() if len(rates) >= 5}
    if not complete:
        raise RatesMarketUnavailable(
            "No SOFR swap curve in the store (run data-ingest's dtcc-swap-rates source)"
        )
    as_of = max(complete)
    age = (today - as_of).days
    if age > MARKET_STALE_AFTER_DAYS:
        raise RatesMarketUnavailable(
            f"The latest SOFR swap curve is from {as_of.isoformat()}, {age} days ago: "
            "data-ingest has not run, or DTCC has not published since"
        )
    window_start = as_of - timedelta(days=MARKET_WINDOW_DAYS)
    trades = [
        t
        for t in db.dtcc_swaption_trades(SOFR_SWAP, window_start)
        if t.report_date <= as_of
    ]
    grid = swaption_market.atm_normal_vols(trades, complete)
    if len(grid.points) < _MIN_VOL_POINTS:
        raise RatesMarketUnavailable(
            f"Only {len(grid.points)} swaption vol points could be built from the "
            f"{len(trades)} trades reported since {window_start.isoformat()}: "
            "not enough to calibrate a model"
        )

    swap_rates = complete[as_of]
    quotes = [{"tenor": float(r.tenor_years), "rate": r.rate} for r in swap_rates]
    ten_year = min(swap_rates, key=lambda r: abs(r.tenor_years - 10))
    # The swaption shown by default: the most traded point of the grid.
    shown = max(grid.points, key=lambda p: p.trades)
    shift = 0.01
    return {
        "currency": "USD",
        "label": (
            f"USD SOFR, from the swaps and swaptions traded and published by DTCC: swap "
            f"curve of {as_of.isoformat()}, swaption vols from {grid.trades_used} trades "
            f"since {window_start.isoformat()}. Medians of trades, not dealer quotes."
        ),
        "as_of": as_of,
        "window_start": window_start,
        # SOFR swaps pay both legs annually, and one curve both discounts and
        # projects: the "index" curve is bootstrapped on the same quotes.
        "fixed_frequency": 1,
        "float_frequency": 1,
        "deposits": [],
        "ois": quotes,
        "fras": [],
        "swaps": quotes,
        "swaption_vols": [
            {"expiry": p.expiry, "tenor": float(p.tenor), "normal_vol": p.normal_vol}
            for p in grid.points
        ],
        "hull_white_mean_reversion": None,
        "swap": {
            "start": 0.0,
            "tenor": float(ten_year.tenor_years),
            "fixed_rate": round(ten_year.rate, 4),
            "notional": 10_000_000.0,
            "payer": True,
        },
        "swaption": {
            "expiry": shown.expiry,
            "tenor": float(shown.tenor),
            "strike": None,
            "payer": True,
            "notional": 10_000_000.0,
            "normal_vol": None,
            # The lognormal and SABR inputs that correspond to the normal vol
            # at the money: σ_N ≈ σ_LN (F + shift) ≈ α (F + shift)^β.
            "lognormal_vol": round(shown.normal_vol / (ten_year.rate + shift), 4),
            "shift": shift,
            "sabr": {
                "alpha": round(shown.normal_vol / math.sqrt(ten_year.rate + shift), 4),
                "beta": 0.5,
                "rho": -0.2,
                "nu": 0.3,
            },
        },
        "swap_rates": [
            {"tenor": float(r.tenor_years), "rate": r.rate, "trades": r.trades}
            for r in swap_rates
        ],
        "market_vols": [
            {
                "expiry": p.expiry,
                "tenor": float(p.tenor),
                "normal_vol": p.normal_vol,
                "low": p.low,
                "high": p.high,
                "trades": p.trades,
            }
            for p in grid.points
        ],
        "trades_used": grid.trades_used,
        "rejected": [
            {"reason": reason, "trades": count}
            for reason, count in sorted(grid.rejected.items(), key=lambda kv: -kv[1])
        ],
    }


__all__ = [
    "analyse",
    "example_request",
    "market_request",
    "methodology",
    "EXAMPLE",
    "RatesMarketUnavailable",
]
