"""Rates derivatives (blueprint/wp/21-rates.md): the quote sets, the curves
built from them, and the swap and the swaption the pricing workbench prices
on them.

From a set of quotes: the OIS discount curve, the projection curve of a
floating index bootstrapped on it (multi-curve), a vanilla swap on the two
curves, a Hull-White model calibrated to a grid of ATM swaption normal vols,
and a swaption under Bachelier, shifted Black, shifted SABR and Hull-White —
European, and Bermudan on the same swap.

**Two sets of quotes.** USD SOFR is market data: the swaps and swaptions
actually traded, published by DTCC and stored by `data-ingest`
(`market_request`, `swaption_market.py`); the Market page shows them. The EUR
set (€STR OIS, EURIBOR 6M) is illustrative and flagged as such: no free source
publishes an IBOR swap curve, and it is the one place where the two curves
differ. Either way a price is a function of the quotes its request carries,
which the user can edit: nothing is read from the store while pricing, so a
recorded valuation replays on its own request (`valuation.py`).
The C++ does the work (`qm.bootstrap_ois_curve`, `bootstrap_projection_curve`,
`price_swap`, `calibrate_hull_white`, `price_swaption`); this module shapes it
and carries the methodology served with the quotes and the curves.
"""

from __future__ import annotations

import math
from datetime import date, timedelta
from functools import lru_cache
from typing import Dict, List, Optional, Tuple

import quantmodeling as qm

from . import db, swaption_market
from .rates_derivatives_schemas import (
    CalibrationPoint,
    CurvePoint,
    CurvesResult,
    HullWhiteCalibrationResult,
    RatesCurveQuotes,
    RatesQuoteSetResponse,
    RatesTradeStats,
    SwapPeriod,
    SwapPricingRequest,
    SwapPricingResponse,
    SwapResult,
    SwaptionInput,
    SwaptionModelPrice,
    SwaptionPricingRequest,
    SwaptionPricingResponse,
    SwaptionResult,
)
from .schemas import Greeks, MethodologySection, ModelWarning

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


def _bootstrap(q: RatesCurveQuotes) -> Tuple[dict, dict]:
    """The OIS curve, then the index curve bootstrapped on it."""
    ois = qm.bootstrap_ois_curve(
        [(d.tenor, d.rate) for d in q.deposits], [(s.tenor, s.rate) for s in q.ois]
    )
    proj = qm.bootstrap_projection_curve(
        ois["times"],
        ois["discount_factors"],
        [(f.start, f.end, f.rate) for f in q.fras],
        [(s.tenor, s.rate) for s in q.swaps],
        q.fixed_frequency,
        q.float_frequency,
    )
    return ois, proj


def curves(q: RatesCurveQuotes) -> CurvesResult:
    """The two curves on a quarterly grid, with the basis between their
    forwards and how well the input swaps reprice."""
    ois, proj = _bootstrap(q)
    last = max(ois["times"][-1], proj["times"][-1])
    grid = [t for t in _GRID if t <= last + 1e-9]
    period = 1.0 / q.float_frequency
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
    for s in q.swaps:
        v = qm.price_swap(
            ois["times"],
            ois["discount_factors"],
            proj["times"],
            proj["discount_factors"],
            0.0,
            s.tenor,
            s.rate,
            q.fixed_frequency,
            q.float_frequency,
        )
        worst = max(worst, abs(v["par_rate"] - s.rate) * 1e4)
    return CurvesResult(
        ois_pillars=ois["times"],
        index_pillars=proj["times"],
        points=points,
        max_repricing_error_bp=worst,
        single_curve=all(abs(p.basis_bp) < 1e-6 for p in points),
    )


def _curve_note(q: RatesCurveQuotes, ois: dict, proj: dict) -> str:
    return (
        f"OIS discounting on {len(ois['times'])} pillars, index projection on "
        f"{len(proj['times'])}; fixed leg {q.fixed_frequency}/y, floating leg "
        f"{q.float_frequency}/y"
    )


# ── Swap ─────────────────────────────────────────────────────────────────────


def price_swap(req: SwapPricingRequest) -> SwapPricingResponse:
    q, s = req.curves, req.swap
    ois, proj = _bootstrap(q)
    v = qm.price_swap(
        ois["times"],
        ois["discount_factors"],
        proj["times"],
        proj["discount_factors"],
        s.start,
        s.tenor,
        s.fixed_rate,
        q.fixed_frequency,
        q.float_frequency,
        s.notional,
        s.payer,
        0.0,
    )
    # Cash-flow sign for the holder: the leg paid is negative.
    side = 1.0 if s.payer else -1.0

    def period(sign: float, rate: float, row: tuple) -> SwapPeriod:
        start, end, payment_time, accrual, discount = row[:5]
        return SwapPeriod(
            start=start,
            end=end,
            payment_time=payment_time,
            accrual=accrual,
            discount=discount,
            rate=rate,
            present_value=sign * rate * accrual * s.notional * discount,
        )

    return SwapPricingResponse(
        npv=v["npv"],
        greeks=Greeks(),
        diagnostics="Multi-curve swap. " + _curve_note(q, ois, proj),
        mc_std_error=0.0,
        swap=SwapResult(
            par_rate=v["par_rate"],
            annuity=v["annuity"],
            pv01=v["pv01"],
            fixed_leg=-side * v["fixed_leg"],
            floating_leg=side * v["floating_leg"],
            fixed_periods=[
                period(-side, s.fixed_rate, row) for row in v["fixed_periods"]
            ],
            floating_periods=[
                period(side, row[5], row) for row in v["floating_periods"]
            ],
        ),
    )


# ── Hull-White calibration ───────────────────────────────────────────────────

_Floats = Tuple[float, ...]


@lru_cache(maxsize=32)
def _calibrate(
    ois_t: _Floats,
    ois_d: _Floats,
    proj_t: _Floats,
    proj_d: _Floats,
    vols: Tuple[Tuple[float, float, float], ...],
    fixed_frequency: int,
    float_frequency: int,
    mean_reversion: Optional[float],
) -> dict:
    """About a second of C++, and the same for every strike, side or notional
    priced on one set of quotes: kept by its inputs."""
    return qm.calibrate_hull_white(
        list(ois_t),
        list(ois_d),
        list(proj_t),
        list(proj_d),
        list(vols),
        fixed_frequency,
        float_frequency,
        mean_reversion,
    )


def _calibration(
    req: SwaptionPricingRequest, ois: dict, proj: dict
) -> HullWhiteCalibrationResult:
    c = _calibrate(
        tuple(ois["times"]),
        tuple(ois["discount_factors"]),
        tuple(proj["times"]),
        tuple(proj["discount_factors"]),
        tuple((v.expiry, v.tenor, v.normal_vol) for v in req.swaption_vols),
        req.curves.fixed_frequency,
        req.curves.float_frequency,
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
                expiry=v.expiry,
                tenor=v.tenor,
                strike=k,
                market_vol=mv,
                model_vol=(
                    None if model is None or not math.isfinite(model) else model
                ),
            )
            for v, k, mv, model in zip(
                req.swaption_vols, c["strikes"], c["market_vols"], c["model_vols"]
            )
        ],
    )


# ── Swaption ─────────────────────────────────────────────────────────────────

_MODEL_NAMES = {
    "bachelier": "Bachelier",
    "black": "shifted Black",
    "sabr": "shifted SABR",
    "hull_white": "Hull-White",
}

_AUTO_EUROPEAN = (
    "A European swaption depends on one swap rate at one date, and the market "
    "quotes it as a normal (Bachelier) vol: the model is the quote convention "
    "itself, priced at the quoted at-the-money vol nearest to this expiry and "
    "tenor."
)
_AUTO_BERMUDAN = (
    "A Bermudan can be exercised on several dates, so its value depends on how "
    "the whole curve moves between them, which a model of one swap rate does "
    "not describe. Hull-White, a one-factor model of the short rate, is "
    "calibrated to the swaption vols and prices it by backward induction."
)
_BY_HAND = "Chosen by hand."


def swaption_model(sw: SwaptionInput) -> str:
    """The model a swaption's value is under: the one asked for, or for
    `auto` the simplest that describes what the price depends on."""
    if sw.model != "auto":
        return sw.model
    return "hull_white" if sw.exercise == "bermudan" else "bachelier"


def swaption_engine(sw: SwaptionInput) -> str:
    return "lattice" if sw.exercise == "bermudan" else "analytic"


def _nearest_vol(req: SwaptionPricingRequest) -> float:
    """The quoted ATM normal vol closest to the swaption's (expiry, tenor):
    the Bachelier price's vol when the request gives none."""
    sw = req.swaption
    best = min(
        req.swaption_vols,
        key=lambda v: (v.expiry - sw.expiry) ** 2 + (v.tenor - sw.tenor) ** 2,
    )
    return best.normal_vol


def price_swaption(req: SwaptionPricingRequest) -> SwaptionPricingResponse:
    q, sw = req.curves, req.swaption
    ois, proj = _bootstrap(q)
    hw = _calibration(req, ois, proj)
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
        q.fixed_frequency,
        q.float_frequency,
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

    def add(key: str, model: str, detail: str) -> None:
        if key not in out:
            return
        price = out[key]
        # The C++ prices on annuity × notional: invert on the same.
        implied = qm.bachelier_implied_vol(
            sw.payer, price, forward, strike, sw.expiry, annuity * sw.notional
        )
        prices.append(
            SwaptionModelPrice(
                key=key,
                model=model,
                price=price,
                implied_normal_vol=implied if math.isfinite(implied) else None,
                detail=detail,
            )
        )

    add(
        "bachelier",
        "Bachelier (normal)",
        f"σ_N = {normal_vol * 1e4:.1f}bp"
        + (" (nearest quoted ATM vol)" if sw.normal_vol is None else ""),
    )
    if sw.lognormal_vol is not None:
        add(
            "black",
            "Black (shifted lognormal)",
            f"σ = {sw.lognormal_vol:.1%}, shift {sw.shift:.2%}",
        )
    if sw.sabr is not None:
        add(
            "sabr",
            "SABR (shifted, Hagan 2002)",
            f"implied lognormal vol {out['sabr_lognormal_vol']:.1%}",
        )
    add(
        "hull_white",
        "Hull-White (calibrated)",
        f"a = {hw.mean_reversion:.4f}, σ = {hw.sigma * 1e4:.1f}bp",
    )

    model = swaption_model(sw)
    bermudan = out.get("hull_white_bermudan")
    european_hw = out.get("hull_white")
    if sw.exercise == "bermudan":
        if bermudan is None:
            raise RuntimeError("the Bermudan swaption could not be priced")
        npv = bermudan
    else:
        npv = next(p.price for p in prices if p.key == model)

    warnings: List[ModelWarning] = []
    off_the_money_bp = abs(strike - forward) * 1e4
    if model == "bachelier" and sw.normal_vol is None and off_the_money_bp > 1.0:
        warnings.append(
            ModelWarning(
                code="atm_vol_off_the_money",
                severity="warning",
                message=(
                    f"The strike is {off_the_money_bp:.0f} bp away from the forward "
                    "swap rate, and the vol is an at-the-money quote: the quotes "
                    "carry no smile, so the price ignores it."
                ),
            )
        )
    if model == "hull_white":
        warnings.append(
            ModelWarning(
                code="hull_white_fit",
                severity="info" if hw.converged else "warning",
                message=(
                    f"Hull-White has two parameters for {len(hw.points)} swaption "
                    f"vols: it fits them to {hw.rmse_bp:.1f} bp of normal vol on "
                    f"average ({hw.worst_bp:.1f} bp at worst)"
                    + ("." if hw.converged else ", and the solver did not converge.")
                ),
            )
        )

    return SwaptionPricingResponse(
        npv=npv,
        greeks=Greeks(),
        diagnostics=(
            f"{sw.exercise.capitalize()} swaption under {_MODEL_NAMES[model]}. "
            + _curve_note(q, ois, proj)
        ),
        mc_std_error=0.0,
        warnings=warnings,
        swaption=SwaptionResult(
            exercise=sw.exercise,
            requested=sw.model,
            model=model,
            reason=(
                _BY_HAND
                if sw.model != "auto"
                else _AUTO_BERMUDAN if sw.exercise == "bermudan" else _AUTO_EUROPEAN
            ),
            forward=forward,
            annuity=annuity,
            strike=strike,
            prices=prices,
            bermudan_price=bermudan,
            bermudan_exercises=exercises,
            switch_premium=(
                None
                if bermudan is None or european_hw is None
                else bermudan - european_hw
            ),
            hull_white=hw,
        ),
    )


# ── Methodology ──────────────────────────────────────────────────────────────


def _sections(sections: List[Tuple[str, List[str]]]) -> List[MethodologySection]:
    return [MethodologySection(title=t, paragraphs=p) for t, p in sections]


def curves_methodology() -> List[MethodologySection]:
    """Served with the curves: how they are built."""
    return _sections(
        [
            (
                "Two curves, not one",
                [
                    "A collateralised swap is discounted at the collateral rate, the "
                    "overnight rate (€STR, SOFR): the OIS curve. Since 2008 a 6-month "
                    "index fixes above the compounded overnight rate by a basis that "
                    "one curve cannot hold, so the index gets its own projection "
                    "curve, bootstrapped from FRAs and par swaps priced with OIS "
                    "discounting (Ametrano & Bianchetti, 2013). When the index is the "
                    "overnight rate itself, as in a SOFR swap, the two curves are the "
                    "same and the basis is zero.",
                    "The OIS curve bootstraps like par bonds: the compounded overnight "
                    "leg of a spot OIS is worth 1 − P(T) on its own curve. Each "
                    "projection pillar is then solved so that its quote reprices "
                    "exactly, with every earlier pillar fixed; the maximum repricing "
                    "error of the input swaps is shown with the curves. Discount "
                    "factors are interpolated log-linearly (piecewise-flat forwards, "
                    "always positive discount factors) and the forward is held flat "
                    "outside the pillars.",
                ],
            )
        ]
    )


def quotes_methodology(set_id: str) -> List[MethodologySection]:
    """Served with a quote set: where its numbers come from."""
    if set_id == "eur-illustrative":
        return _sections(
            [
                (
                    "Where the quotes come from",
                    [
                        "No free source publishes dealer quotes of OIS swap rates, the "
                        "swap curve of a term index such as EURIBOR, or swaption "
                        "volatilities. These are illustrative EUR quotes, which are "
                        "not market data: they are here because they are the one set "
                        "where the index curve differs from the OIS curve. Replace "
                        "them with your own. The government curves of the Market page "
                        "are bond yields: a different curve, not an input here.",
                    ],
                )
            ]
        )
    return _sections(
        [
            (
                "Where the swap curve and the swaption vols come from",
                [
                    "This is market data of a particular kind: not quotes but trades. "
                    "US swap dealers must report every swap to a repository, which "
                    "publishes its price and size (CFTC public dissemination; DTCC's "
                    "repository, stored daily). The swap curve is, per tenor, the "
                    "median fixed rate of the spot-starting swaps traded on the latest "
                    "day. Each swaption vol is the median, over the last days, of the "
                    "normal vols implied by the traded premiums: premium / notional = "
                    "annuity × Bachelier(forward, strike, expiry, σ), solved for σ on "
                    "the swap curve of the trade's day. The number of trades behind "
                    "each point is shown.",
                    "What is left out, and why: capped notionals (the premium is "
                    "published in full, the notional is not); novations and amendments "
                    "(not prices); trades away from the money (the files do not say "
                    "whether a call is a payer or a receiver, and only at the money "
                    "does it not matter); expiries beyond two years (their premium "
                    "appears to be paid at expiry rather than up front, which the "
                    "files do not say). A straddle traded on a platform carries the "
                    "premium of both legs on each leg and counts for half. These are "
                    "medians of a handful of trades, not a dealer's surface: read the "
                    "trade counts.",
                ],
            )
        ]
    )


def example_request() -> Dict:
    """The illustrative EUR set, with the contracts the workbench opens on."""
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


# ── Quote sets ───────────────────────────────────────────────────────────────


def quote_set(set_id: str, today: Optional[date] = None) -> RatesQuoteSetResponse:
    """A quote set in the shape the workbench prices on: the curve quotes,
    the swaption vols, the contracts it opens on, and where it all comes
    from. `usd-sofr` raises RatesMarketUnavailable when the store cannot
    back it."""
    market = set_id == "usd-sofr"
    m = market_request(today) if market else example_request()
    return RatesQuoteSetResponse(
        id=set_id,
        currency=m["currency"],
        label=m["label"],
        source="market" if market else "manual",
        as_of=m.get("as_of"),
        curves=RatesCurveQuotes(
            fixed_frequency=m["fixed_frequency"],
            float_frequency=m["float_frequency"],
            deposits=m["deposits"],
            ois=m["ois"],
            fras=m["fras"],
            swaps=m["swaps"],
        ),
        swaption_vols=m["swaption_vols"],
        swap=m["swap"],
        swaption=m["swaption"],
        trades=(
            RatesTradeStats(
                window_start=m["window_start"],
                swap_rates=m["swap_rates"],
                swaption_vols=m["market_vols"],
                trades_used=m["trades_used"],
                rejected=m["rejected"],
            )
            if market
            else None
        ),
        methodology=quotes_methodology(set_id),
    )


__all__ = [
    "curves",
    "curves_methodology",
    "example_request",
    "market_request",
    "price_swap",
    "price_swaption",
    "quote_set",
    "quotes_methodology",
    "swaption_engine",
    "swaption_model",
    "EXAMPLE",
    "RatesMarketUnavailable",
]
