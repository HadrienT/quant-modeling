"""xVA of a netting set — exposure, collateral, CVA and DVA on market inputs
(blueprint/wp/23-xva.md, lot X3).

A teaching tool on real data (§13.7 of the work package): a handful of
portfolios, from one swap to a hedged book, each showing one idea of Gregory's
*The xVA Challenge*. Everything the computation rests on is read from the
store, with no fallback:

- the USD SOFR swap curve and the swaption vols Hull-White is calibrated on,
  both from traded prices (`rates_derivatives.market_request`);
- the credit curves of the counterparty and of the bank, proxied by rating
  from the ICE BofA option-adjusted spreads (as on the credit page);
- the historical dynamics of the short rate, estimated on the 3-month
  Treasury yield, for the risk measures — with the user free to override
  them, because the data pin down the volatility and little else.

The C++ does the work in one call (`qm.xva_netting_set`); this module builds
the portfolios, gathers the inputs and carries the methodology.
"""

from __future__ import annotations

import math
import time
from dataclasses import dataclass
from datetime import date
from typing import Dict, List, Optional, Tuple

import quantmodeling as qm

from . import credit, db, rates_derivatives
from .schemas import MethodologySection
from .xva_schemas import (
    Adjustments,
    CapitalOut,
    CreditInput,
    Estimate,
    ExposureProfileOut,
    HistoricalDynamics,
    HullWhiteInput,
    InitialMarginOut,
    PricingExposure,
    XvaMarket,
    XvaRequest,
    XvaResponse,
    XvaTrade,
)

#: Notional of the reference trade of every portfolio, USD.
NOTIONAL = 10_000_000.0
#: The series the historical dynamics are estimated on, and from when.
HISTORY_SERIES = "DGS3MO"
HISTORY_SINCE = date(1990, 1, 1)
_BUSINESS_DAYS = 250.0


class XvaUnavailable(RuntimeError):
    """An input the computation needs is not in the store."""


class XvaInputError(ValueError):
    """A request the computation cannot honour as asked."""


# ── Portfolios ───────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class _Spec:
    description: str
    kind: str  # "swap" | "swaption" | "bermudan"
    payer: bool
    tenor: float
    notional: float
    #: The expiry of a swaption; the first exercise date of a Bermudan.
    start: float = 0.0
    quantity: float = 1.0
    #: (start, tenor) of the swap whose par rate is the fixed rate, when it is
    #: not the trade's own swap: the right to cancel a swap is struck at that
    #: swap's rate.
    par_of: Optional[Tuple[float, float]] = None


def _swap(tenor: float, payer: bool, notional: float = NOTIONAL) -> _Spec:
    side = "Payer" if payer else "Receiver"
    return _Spec(f"{side} swap {tenor:g}Y at par", "swap", payer, tenor, notional)


def _swaption(
    expiry: float, tenor: float, bought: bool, notional: float = NOTIONAL
) -> _Spec:
    side = "Bought" if bought else "Sold"
    return _Spec(
        f"{side} payer swaption {expiry:g}Y into {tenor:g}Y, at the money",
        "swaption",
        True,
        tenor,
        notional,
        start=expiry,
        quantity=1.0 if bought else -1.0,
    )


def _bermudan(
    first: float,
    tenor: float,
    payer: bool,
    notional: float = NOTIONAL,
    par_of: Optional[Tuple[float, float]] = None,
) -> _Spec:
    side = "payer" if payer else "receiver"
    return _Spec(
        f"Bought Bermudan {side} swaption, exercisable each year from {first:g}Y "
        f"into what is left of a swap ending at {first + tenor:g}Y",
        "bermudan",
        payer,
        tenor,
        notional,
        start=first,
        par_of=par_of,
    )


#: id -> (label, what it teaches, trades).
PORTFOLIOS: Dict[str, Tuple[str, str, List[_Spec]]] = {
    "single_swap": (
        "One 10-year payer swap",
        "The exposure of a swap is a hump: rates diffuse like the square root of "
        "time while the remaining coupons run off. CVA is close to the credit "
        "spread times the average exposure times the maturity.",
        [_swap(10, True)],
    ),
    "bought_swaption": (
        "One bought swaption",
        "An option that was paid for can only be an asset: until it expires its "
        "discounted expected exposure is its price, and the CVA of that period "
        "is the loss given default times the price times the probability of "
        "default. Once exercised it is a swap.",
        [_swaption(1, 10, True)],
    ),
    "sold_swaption": (
        "One sold swaption",
        "The seller received the premium and can only owe: no exposure and no "
        "CVA until the expiry, only a DVA. After a physical exercise it is a "
        "swap, which can turn either way.",
        [_swaption(1, 10, False)],
    ),
    "directional": (
        "A directional book: four payer swaps",
        "Trades that move together do not net: the exposure of the set is "
        "almost the sum of the exposures of its trades, and every trade adds "
        "to the CVA.",
        [_swap(2, True), _swap(5, True), _swap(10, True), _swap(20, True)],
    ),
    "balanced": (
        "A balanced book: payers, receivers and an option",
        "Close-out netting: trades that offset each other reduce the exposure "
        "of the set well below the sum of its trades. A trade can have a "
        "positive incremental CVA: it lowers the counterparty risk of what is "
        "already there.",
        [
            _swap(10, True),
            _swap(10, False, 0.8 * NOTIONAL),
            _swap(5, True, 0.6 * NOTIONAL),
            _swap(7, False, 0.6 * NOTIONAL),
            _swap(2, False),
            _swaption(2, 10, True, 0.5 * NOTIONAL),
        ],
    ),
    "bermudan": (
        "One bought Bermudan swaption",
        "A Bermudan can be exercised on several dates, so no formula gives what "
        "it will be worth: its future value is estimated by regression on the "
        "simulated rates (American Monte-Carlo). Until it is exercised it is an "
        "asset, like any bought option. Each path that exercises becomes a swap, "
        "which can turn either way.",
        [_bermudan(1, 9, True)],
    ),
    "cancellable": (
        "A cancellable swap",
        "A 10-year payer swap with the right to cancel it on each anniversary: "
        "that right is a Bermudan receiver swaption on what is left of the swap. "
        "The bank cancels when the swap has turned against it, so the right "
        "removes what the bank would owe, not what it is owed: the DVA shrinks, "
        "and the option, an asset, adds its own exposure.",
        [_swap(10, True), _bermudan(1, 9, False, par_of=(0.0, 10.0))],
    ),
}

_RATING_SERIES = {
    ("CCC" if r.label.startswith("CCC") else r.label): r.series_id
    for r in credit.RATINGS
}
RATINGS: List[str] = list(_RATING_SERIES)

# ── Market inputs ────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class _Rates:
    as_of: date
    label: str
    times: List[float]
    dfs: List[float]
    mean_reversion: float
    sigma: float
    rmse_bp: float
    vol_points: int
    swaption_trades: int
    quotes: List[Tuple[float, float]]
    vols: List[Tuple[float, float, float]]


def _rates_market(today: Optional[date]) -> _Rates:
    try:
        m = rates_derivatives.market_request(today)
    except rates_derivatives.RatesMarketUnavailable as exc:
        raise XvaUnavailable(str(exc)) from exc
    quotes = [(q["tenor"], q["rate"]) for q in m["ois"]]
    vols = [(v["expiry"], v["tenor"], v["normal_vol"]) for v in m["swaption_vols"]]
    curve = qm.bootstrap_ois_curve([], quotes)
    t, d = curve["times"], curve["discount_factors"]
    hw = qm.calibrate_hull_white(t, d, t, d, vols, 1, 1, None)
    return _Rates(
        m["as_of"],
        m["label"],
        list(t),
        list(d),
        hw["mean_reversion"],
        hw["sigma"],
        hw["rmse_bp"],
        len(vols),
        m["trades_used"],
        quotes,
        vols,
    )


def _credit(
    rating: str, recovery: float, rates: _Rates, warnings: List[str]
) -> CreditInput:
    """The flat hazard rate that reprices a five-year CDS at the rating's
    spread, as on the credit page."""
    snap = db.rates_curve_snapshot("fred", list(_RATING_SERIES.values()))
    if snap is None:
        raise XvaUnavailable(
            "No date on which every ICE BofA rating spread is in the store "
            "(run data-ingest's fred-macro source)"
        )
    as_of, values = snap
    stale = credit.staleness_warning(as_of, "credit spreads by rating")
    if stale and stale not in warnings:
        warnings.append(stale)
    spread = values[_RATING_SERIES[rating]] / 100.0
    boot = qm.bootstrap_credit_curve(
        [(credit.RATING_HAZARD_MATURITY, spread)], rates.times, rates.dfs, recovery
    )
    return CreditInput(
        rating=rating, spread=spread, hazard=boot["hazards"][0], as_of=as_of
    )


def _historical(req: XvaRequest) -> HistoricalDynamics:
    series = db.fred_series(HISTORY_SERIES, HISTORY_SINCE)
    if len(series) < 500:
        raise XvaUnavailable(
            f"The history of {HISTORY_SERIES} is not in the store "
            "(run data-ingest's fred-macro source with --full)"
        )
    values = [float(v) / 100.0 for v in series.to_list()]
    dt = 1.0 / _BUSINESS_DAYS
    sigma = qm.estimate_historical_volatility(values, dt)
    a = theta = a_error = theta_error = None
    try:
        e = qm.estimate_historical_rate_dynamics(values, dt)
        a, theta, sigma = e["mean_reversion"], e["long_run_rate"], e["sigma"]
        a_error, theta_error = (
            e["mean_reversion_std_error"],
            e["long_run_rate_std_error"],
        )
    except RuntimeError:
        pass  # no measurable mean reversion: a and θ must come from the request
    h = req.historical
    if (a is None and h.mean_reversion is None) or (
        theta is None and h.long_run_rate is None
    ):
        raise XvaUnavailable(
            f"No mean reversion can be measured on {HISTORY_SERIES} since "
            f"{HISTORY_SINCE.isoformat()}: set the mean reversion and the long-run rate"
        )
    overridden = [
        name
        for name, given in (
            ("mean_reversion", h.mean_reversion),
            ("long_run_rate", h.long_run_rate),
            ("sigma", h.sigma),
        )
        if given is not None
    ]
    return HistoricalDynamics(
        mean_reversion=h.mean_reversion if h.mean_reversion is not None else a,
        long_run_rate=h.long_run_rate if h.long_run_rate is not None else theta,
        sigma=h.sigma if h.sigma is not None else sigma,
        estimated_mean_reversion=a,
        estimated_long_run_rate=theta,
        estimated_sigma=sigma,
        mean_reversion_std_error=a_error,
        long_run_rate_std_error=theta_error,
        series=HISTORY_SERIES,
        since=series.index[0].date(),
        observations=len(values),
        overridden=overridden,
    )


# ── Computation ──────────────────────────────────────────────────────────────


def _trades(specs: List[_Spec], rates: _Rates) -> List[dict]:
    """The trades at the market: each fixed rate is the par rate of its swap
    on today's curve (the forward par rate for a swaption)."""
    out = []
    for s in specs:
        start, tenor = s.par_of or (s.start, s.tenor)
        par = qm.price_swap(
            rates.times,
            rates.dfs,
            rates.times,
            rates.dfs,
            start,
            tenor,
            0.0,
            1,
            1,
            1.0,
            True,
            0.0,
        )["par_rate"]
        trade = {
            "kind": s.kind,
            "tenor": s.tenor,
            "fixed_rate": par,
            "notional": s.notional,
            "payer": s.payer,
            "quantity": s.quantity,
            "fixed_frequency": 1,
            "float_frequency": 1,
        }
        trade["start" if s.kind == "swap" else "expiry"] = s.start
        out.append(trade)
    return out


def _profile(e: dict) -> dict:
    return {k: e[k] for k in ("times", "ee", "ene", "pfe", "epe", "eepe")}


def _pricing_exposure(e: dict) -> PricingExposure:
    return PricingExposure(
        **_profile(e),
        discounted_ee=e["discounted_ee"],
        discounted_ene=e["discounted_ene"],
        discounted_ee_error=e["discounted_ee_error"],
    )


def _adjustments(r: dict) -> Adjustments:
    return Adjustments(
        cva=Estimate(**r["cva"]),
        dva=Estimate(**r["dva"]),
        cva_independent=Estimate(**r["cva_independent"]),
        dva_independent=Estimate(**r["dva_independent"]),
        cva_unilateral=r["cva_unilateral"],
        fca=r["fca"],
        fba=r["fba"],
        colva=r["colva"],
        mva=r["mva"],
        kva=r["kva"],
        cva_rule_of_thumb=r["cva_rule_of_thumb"],
    )


def _margin_model(req: XvaRequest, specs: List[_Spec]) -> Tuple[str, str]:
    """The model of the initial margin, and why. SIMM needs the sensitivities
    of every trade in every scenario, which the closed forms give for swaps
    and European swaptions; a Bermudan is valued by regression and has none."""
    asked = req.csa.initial_margin_model
    by_regression = sorted({s.kind for s in specs} - {"swap", "swaption"})
    if req.csa.initial_margin_today is not None:
        if asked == "simm":
            raise XvaInputError(
                "An initial margin given for today goes with the regression model, "
                "which it scales; SIMM computes today's margin itself."
            )
        return "regression", (
            "The margin you gave for today is the starting point: the regression "
            "model gives the shape of the profile, scaled to start from it."
        )
    if asked == "regression":
        return "regression", "Chosen by hand."
    if by_regression:
        if asked == "simm":
            raise XvaInputError(
                "SIMM on every path needs the sensitivities of each trade in each "
                f"scenario; a {by_regression[0]} is valued by regression and has none. "
                "Use the regression model for this portfolio."
            )
        return "regression", (
            f"This portfolio holds a {by_regression[0]}, valued by regression: it has "
            "no sensitivity to each point of the curve in each scenario, which SIMM "
            "needs. The margin is modelled instead: the 99 % quantile of the move of "
            "the value over ten days, fitted on the simulated paths."
        )
    return "simm", (
        "Chosen by hand."
        if asked == "simm"
        else "Every trade is a swap or a European swaption, whose sensitivities are "
        "known in closed form in each scenario: the margin is ISDA SIMM itself, the "
        "rule the industry uses, rather than a model of it."
    )


#: The PD floor of the IRB formula (Basel framework, CRE32.4).
_PD_FLOOR = 0.0005
_INVESTMENT_GRADE = {"AAA", "AA", "A", "BBB"}


def _capital_inputs(req: XvaRequest, counterparty: CreditInput) -> dict:
    """What the capital projection takes: the counterparty's regulatory
    parameters and the cost of capital."""
    c = req.capital
    # A rating's spread implies a default intensity under the pricing
    # measure, above the default rates a bank's rating system would estimate:
    # used only when no PD is given, and flagged.
    pd = c.pd if c.pd is not None else 1.0 - math.exp(-counterparty.hazard)
    return {
        "pd": pd,
        # Foundation approach, CRE32.6.
        "lgd": (
            c.lgd if c.lgd is not None else (0.45 if c.sector == "financial" else 0.40)
        ),
        "sector": c.sector,
        "investment_grade": counterparty.rating in _INVESTMENT_GRADE,
        "cost_of_capital": c.cost_of_capital,
    }


def _capital(r: dict, inputs: dict, req: XvaRequest, margined: bool) -> CapitalOut:
    k = r["capital"]
    return CapitalOut(
        ead_today=k["ead_today"],
        default_capital_today=k["default_capital_today"],
        cva_capital_today=k["cva_capital_today"],
        times=k["times"],
        expected_ead=k["expected_ead"],
        discounted_capital=k["discounted_capital"],
        pd=max(inputs["pd"], _PD_FLOOR),
        pd_is_market_implied=req.capital.pd is None,
        lgd=inputs["lgd"],
        sector=inputs["sector"],
        investment_grade=inputs["investment_grade"],
        margined=margined,
        cost_of_capital=inputs["cost_of_capital"],
    )


def compute(req: XvaRequest, today: Optional[date] = None) -> XvaResponse:
    started = time.perf_counter()
    warnings: List[str] = []
    label, lesson, specs = PORTFOLIOS[req.portfolio]
    rates = _rates_market(today)
    counterparty = _credit(req.counterparty_rating, req.recovery, rates, warnings)
    own = _credit(req.own_rating, req.recovery, rates, warnings)
    historical = _historical(req)
    trades = _trades(specs, rates)

    csa = None
    if req.csa is not None:
        csa = {
            "threshold_counterparty": req.csa.threshold_counterparty,
            "threshold_bank": req.csa.threshold_bank,
            "minimum_transfer_amount": req.csa.minimum_transfer_amount,
            "margin_period_of_risk": req.csa.margin_period_of_risk_days
            / _BUSINESS_DAYS,
            "cashflows": req.csa.cashflows,
        }
    lgd = 1.0 - req.recovery
    horizon = [max(s.start + s.tenor for s in specs)]
    margin = None
    margin_model, margin_reason = "regression", ""
    if req.csa is not None and req.csa.initial_margin:
        margin_model, margin_reason = _margin_model(req, specs)
        margin = {
            "confidence": 0.99,
            "im_today": req.csa.initial_margin_today,
            # Segregated margin is funded at the bank's borrowing spread.
            "spread": req.borrowing_spread,
            "model": margin_model,
        }
    capital = _capital_inputs(req, counterparty)
    result = qm.xva_netting_set(
        rates.times,
        rates.dfs,
        (rates.mean_reversion, rates.sigma),
        trades,
        (horizon, [counterparty.hazard]),
        (horizon, [own.hazard]),
        lgd,
        lgd,
        csa,
        req.borrowing_spread,
        req.lending_spread,
        (historical.mean_reversion, historical.long_run_rate, historical.sigma),
        req.paths,
        req.seed,
        req.pfe_confidence,
        initial_margin=margin,
        collateral_spread=req.csa.collateral_rate_spread if req.csa else 0.0,
        capital=capital,
        # Per unit of currency in the library; per reference notional here.
        wrong_way_b=req.wrong_way_risk / NOTIONAL,
        device=req.device,
    )

    warnings.append(
        "Counterparty and own credit curves are rating proxies built from bond "
        "spreads (ICE BofA indices), not CDS quotes of a name."
    )
    if req.capital.pd is None:
        warnings.append(
            "The capital uses the default probability implied by the rating's "
            "spread, which is higher than the default rate a bank's rating system "
            "would estimate: the capital and the KVA are on the high side. Enter "
            "your own one-year PD to replace it."
        )
    if historical.estimated_mean_reversion is not None and not {
        "mean_reversion",
        "long_run_rate",
    } <= set(historical.overridden):
        warnings.append(
            "The long-run rate and the mean reversion of the historical measure are "
            "estimates that change with the window; the risk measures depend on them."
        )
    rows = []
    for spec, trade, value, c in zip(
        specs, trades, result["trade_values_today"], result["contributions"]
    ):
        rows.append(
            XvaTrade(
                description=spec.description,
                kind=spec.kind,
                payer=spec.payer,
                quantity=spec.quantity,
                notional=spec.notional,
                fixed_rate=trade["fixed_rate"],
                start=spec.start,
                tenor=spec.tenor,
                value_today=value,
                standalone_cva=c["standalone_cva"],
                incremental_cva=c["incremental_cva"],
                marginal_cva=c["marginal_cva"],
            )
        )
    open_set = result["uncollateralised"]
    return XvaResponse(
        portfolio=req.portfolio,
        portfolio_label=label,
        lesson=lesson,
        value_today=sum(result["trade_values_today"]),
        trades=rows,
        exposure=_pricing_exposure(result["exposure"]),
        exposure_uncollateralised=(
            _pricing_exposure(open_set["exposure"]) if open_set else None
        ),
        risk=ExposureProfileOut(**_profile(result["risk"])),
        adjustments=_adjustments(result),
        adjustments_uncollateralised=_adjustments(open_set) if open_set else None,
        initial_margin=(
            InitialMarginOut(
                today=result["initial_margin"]["today"],
                times=result["exposure"]["times"],
                expected=result["initial_margin"]["expected"],
                requested=req.csa.initial_margin_model,
                model=margin_model,
                reason=margin_reason,
                simm_today=result["simm_today"],
            )
            if result["initial_margin"]
            else None
        ),
        # SA-CCR is margined when the counterparty posts variation margin.
        capital=_capital(result, capital, req, margined=req.csa is not None),
        capital_uncollateralised=(
            _capital(open_set, capital, req, margined=False) if open_set else None
        ),
        market=XvaMarket(
            currency="USD",
            curve_as_of=rates.as_of,
            curve_label=rates.label,
            hull_white=HullWhiteInput(
                mean_reversion=rates.mean_reversion,
                sigma=rates.sigma,
                rmse_bp=rates.rmse_bp,
                vol_points=rates.vol_points,
                swaption_trades=rates.swaption_trades,
            ),
            counterparty=counterparty,
            own=own,
            recovery=req.recovery,
            historical=historical,
        ),
        paths=req.paths,
        pilot_paths=result["pilot_paths"],
        seed=req.seed,
        device=result["device"],
        gpus=result["gpus"],
        device_reason=_device_reason(req.device, result),
        compute_ms=(time.perf_counter() - started) * 1000.0,
        warnings=warnings,
        methodology=methodology(),
    )


def _device_reason(requested: str, result: dict) -> str:
    """Why the paths ran where they did, for the reader of the response."""
    if result["device"] == "gpu":
        cards = result["gpus"]
        return (
            f"Every trade has a closed form: the paths were valued on "
            f"{cards} GPU{'s' if cards > 1 else ''}, with the scenarios the CPU "
            "would have drawn."
        )
    if requested == "cpu":
        return "The CPU was requested."
    return f"On the CPU: {result['device_note']}."


def market_inputs(response: XvaResponse) -> List[Tuple[str, str, str, object]]:
    """(name, source, as_of, value) of what the computation read from the
    store, for the audit record."""
    m = response.market
    return [
        (
            "sofr_swap_curve",
            "db:rates.dtcc_swap_rates",
            m.curve_as_of.isoformat(),
            m.curve_label,
        ),
        (
            "hull_white_calibration",
            "db:rates.dtcc_swaptions",
            m.curve_as_of.isoformat(),
            [m.hull_white.mean_reversion, m.hull_white.sigma],
        ),
        (
            f"credit_spread:{m.counterparty.rating}",
            "db:macro.fred_series",
            m.counterparty.as_of.isoformat(),
            m.counterparty.spread,
        ),
        (
            f"credit_spread:{m.own.rating}",
            "db:macro.fred_series",
            m.own.as_of.isoformat(),
            m.own.spread,
        ),
        (
            f"rate_history:{m.historical.series}",
            "db:macro.fred_series",
            None,
            [
                m.historical.estimated_mean_reversion,
                m.historical.estimated_long_run_rate,
                m.historical.estimated_sigma,
            ],
        ),
    ]


# ── Methodology ──────────────────────────────────────────────────────────────

_SECTIONS: List[Tuple[str, List[str]]] = [
    (
        "What is computed",
        [
            "If a counterparty defaults while a trade is worth something to the bank, "
            "the bank loses that value less what it recovers. The credit valuation "
            "adjustment (CVA) is the price of that risk today; the debit valuation "
            "adjustment (DVA) is the same thing seen by the counterparty about the "
            "bank. Both come from one object: the distribution of the future value "
            "of the netting set, date by date.",
            "Sign convention, Gregory's (4th edition): an adjustment is negative when "
            "it is a cost to the bank. Adjusted value = risk-free value + CVA + DVA "
            "+ FVA. CVA is negative or zero, DVA positive or zero.",
        ],
    ),
    (
        "Exposure",
        [
            "V(t) is the value of the netting set to the bank at a future date, after "
            "the payment of that date. Expected exposure EE(t) is the average of "
            "max(V(t), 0); expected negative exposure ENE(t) the average of "
            "min(V(t), 0), a negative number. Potential future exposure PFE(t) is a "
            "high quantile of V(t). EPE is the time average of EE; the Effective EPE "
            "(EEPE) of the Basel framework averages, over the first year, an EE that "
            "is not allowed to decrease.",
            "The values come from a simulation: thousands of paths of interest rates "
            "on a grid of dates that includes every payment date, and each trade "
            "repriced on each path at each date. Close-out netting is applied path by "
            "path: values add up before the positive part is taken.",
        ],
    ),
    (
        "Interest-rate scenarios",
        [
            "Rates follow the one-factor Hull-White model, dr = (θ(t) − a r)dt + σ dW, "
            "fitted exactly to today's swap curve. In this model the price of every "
            "zero-coupon bond at a future date is a closed formula of one state "
            "variable, so swaps and European swaptions are repriced exactly on each "
            "path, without approximation.",
            "The two parameters are calibrated to the swaptions actually traded: the "
            "mean reversion a and the volatility σ that best reproduce the normal "
            "vols implied by traded premiums (see the Rates page). The swap curve is "
            "the median rate of the swaps traded on the latest day. These are "
            "medians of trades published by DTCC, not dealer quotes.",
        ],
    ),
    (
        "CVA and DVA",
        [
            "CVA = −LGD × Σ EE*(t_i) × S_bank(t_{i−1}) × PD_counterparty(t_{i−1}, t_i), "
            "where EE* is the discounted expected exposure, LGD = 1 − recovery, PD "
            "the probability that the counterparty defaults in the interval and "
            "S_bank the probability that the bank has not defaulted before: only the "
            "first default matters. DVA is the mirror image, on the negative "
            "exposure and the bank's own default. The unilateral CVA ignores the "
            "bank's default.",
            "Each figure carries its Monte-Carlo standard error, computed from the "
            "dispersion of the loss path by path. Rule of thumb shown next to it: "
            "CVA ≈ −credit spread × EPE × maturity.",
            "The model assumes that default is independent of the level of rates (no "
            "wrong-way risk) and that the two defaults are independent of each other.",
        ],
    ),
    (
        "Bermudans: value by regression",
        [
            "A swap and a European swaption have an exact value on every path at "
            "every date under the model. A Bermudan does not: what it is worth "
            "depends on when it will be exercised. Its value is estimated by "
            "American Monte-Carlo (Longstaff & Schwartz, 2001): on a separate, "
            "independent set of pilot paths, the cash flows it goes on to pay are "
            "regressed on the level of rates at each date, which gives its value "
            "as a function of rates. The number of pilot paths is shown with the "
            "result.",
            "Each path carries an exercise state. On an exercise date the holder "
            "enters the swap when it is worth more than keeping the option; from "
            "then on the trade is that swap, valued exactly. The rule is an "
            "estimate of the best one, so the simulated holder is slightly "
            "sub-optimal: the value shown for today is the lattice price, and the "
            "simulated exposure starts just below it. On swaps and European "
            "swaptions, where the exact value is known, the same regression "
            "reproduces the expected exposure within 1 to 2 % of its peak and the "
            "99 % quantile within 2 to 4 %, the more pilot paths the closer.",
        ],
    ),
    (
        "Wrong-way risk",
        [
            "Every figure above assumes that the counterparty's default has "
            "nothing to do with what it owes. Wrong-way risk is the case where it "
            "defaults more often precisely when it owes more. It is modelled as "
            "Hull & White (2012) do: the hazard rate depends on the value of the "
            "netting set, λ(t) = exp(a(t) + b × V(t)), with a(t) solved at each "
            "date so that the survival probability averaged over the scenarios is "
            "still the market's. The parameter moves default probability between "
            "scenarios; it does not create any.",
            "The parameter is entered per reference notional of 10 M: a value of "
            "10 multiplies the hazard by e^0.5 ≈ 1.65 in a scenario where the "
            "netting set is worth 5 % of the notional more. It has no market "
            "quote: it is a scenario, and the figure to read is the ratio of the "
            "CVA with it to the independent CVA, shown side by side. Under a "
            "collateral agreement the effect nearly disappears: the exposure is "
            "then the move of the value over ten days, which is no larger where "
            "the value is high. Funding, margin and capital adjustments keep the "
            "market survival curve.",
        ],
    ),
    (
        "Initial margin and MVA",
        [
            "Under the margin rules for non-cleared derivatives each party also "
            "posts initial margin, held apart, sized to cover 99 % of the move of "
            "the netting set's value over the margin period of risk. Pricing it "
            "needs that margin in every scenario at every date. It is projected by "
            "regression (Anfuso, Aziz, Giltinan & Loukopoulos, 2017): on the "
            "simulated paths, the squared move of the value over the next ten days "
            "is regressed on the value, which gives its conditional standard "
            "deviation; the margin is 2.33 times it, the 99 % quantile of a normal "
            "move. This is a model of the margin, not the industry's rule (ISDA "
            "SIMM works from sensitivities): if you give today's actual margin, the "
            "whole profile is scaled to start from it.",
            "When every trade is a swap or a European swaption, the margin is "
            "instead ISDA SIMM itself (methodology version 2.8+2512, interest rate "
            "risk class), computed in every scenario at every date from the "
            "sensitivities of that scenario: the PV01 at each of the twelve "
            "vertices of the curve, weighted by ISDA's risk weights and aggregated "
            "with its correlations, plus the vega and curvature of the options. The "
            "sensitivities come from the closed forms, not from a bump: each cash "
            "flow's sensitivity to its own zero rate, shared between the two "
            "vertices around it; for an option, its vega at the normal volatility "
            "its price implies. (SIMM asks for sensitivities to market rates; zero "
            "rates are used here.) SIMM is calibrated on a period of stress and "
            "comes out about one and a half times the regression model's margin.",
            "With that margin the exposure is what is left beyond it, on about one "
            "path in a hundred: the CVA almost disappears. What replaces it is the "
            "cost of funding the margin posted, MVA = −Σ E[D × IM](t) × survival of "
            "both parties × funding spread × Δt. Segregated margin funds nothing, so "
            "the funding adjustments stay those of the variation margin.",
        ],
    ),
    (
        "ColVA",
        [
            "A collateral agreement pays a rate on the cash it holds. When that "
            "rate is the overnight rate the trades are discounted at, nothing is "
            "gained or lost: this is why collateralised trades are discounted on "
            "the OIS curve (Piterbarg, 2010). When it differs, ColVA = −Σ E[D × "
            "collateral held](t) × survival of both parties × (rate paid − discount "
            "rate) × Δt: a cost when the bank holds collateral on which it pays "
            "more.",
        ],
    ),
    (
        "Capital and KVA",
        [
            "Two regulatory charges are projected. The exposure at default is the "
            "standardised approach's (SA-CCR, Basel framework CRE52): 1.4 × "
            "(replacement cost + multiplier × add-on). At a future date the add-on "
            "comes from the trades that are left, the same in every scenario; the "
            "replacement cost and the multiplier come from the value, and the "
            "margin held, in the scenario. The default-risk capital is the IRB "
            "formula on that exposure (CRE31, with the PD floor of 0.05 % of "
            "CRE32.4, the foundation LGD of CRE32.6 and an effective maturity "
            "capped at five years, CRE32.46); the CVA capital is the reduced basic "
            "approach (BA-CVA, MAR50), with the sector and investment-grade risk "
            "weight of the counterparty and no cap on the maturity (MAR50.15).",
            "KVA = −Σ E[D × capital](t) × survival of both parties × cost of capital "
            "× Δt (Green, Kenyon & Dennis, 2014). An option keeps today's moneyness "
            "in its supervisory delta, and after its last exercise date it is "
            "carried as its underlying swap. Market-risk capital of the hedges and "
            "the leverage ratio are not included.",
        ],
    ),
    (
        "Credit curves",
        [
            "No free source publishes CDS spreads by name. The counterparty and the "
            "bank are given a rating, and the rating a spread: the option-adjusted "
            "spread of the ICE BofA US corporate index of that rating (FRED). The "
            "hazard rate is the constant default intensity that reprices a five-year "
            "CDS at that spread, about spread / (1 − recovery).",
            "These are bond spreads, which differ from CDS spreads by a basis and "
            "include a liquidity premium: a proxy, as banks use for counterparties "
            "without a traded CDS.",
        ],
    ),
    (
        "Collateral",
        [
            "Under a collateral agreement (CSA) the party that is owed money receives "
            "variation margin: the value above a threshold, called when the change "
            "exceeds a minimum transfer amount. What remains is the margin period of "
            "risk: if the counterparty defaults at t, the collateral in hand is the "
            "one called on the value some days earlier (ten business days by "
            "regulatory convention), and the exposure is V(t) − C(t − MPoR).",
            "Coupons paid during that period matter: where the bank pays a coupon the "
            "value jumps while the collateral still reflects the value before it, "
            "which shows as spikes in the exposure. The treatment can be changed "
            "(both parties keep paying, both stop, or only the bank keeps paying).",
            "The comparison with the uncollateralised netting set is made on the same "
            "paths.",
        ],
    ),
    (
        "Two measures: prices and risks",
        [
            "CVA is a price: its exposure is computed on risk-neutral scenarios, "
            "calibrated to option prices. PFE and credit limits are risk measures — "
            "how large the exposure can really get — and are computed on scenarios "
            "that follow the historical behaviour of rates, dr = a(θ − r)dt + σ dW, "
            "estimated on the 3-month Treasury yield since 1990. In both cases each "
            "trade is valued on each path by the market-calibrated model: only the "
            "probability of the scenarios differs.",
            "The historical volatility σ is well determined by the data. The mean "
            "reversion a and the long-run rate θ are not: they change entirely with "
            "the estimation window, and θ drives the result (a long-run rate below "
            "today's makes payer swaps lose value on average). They are shown with "
            "their standard errors and can be set by hand.",
        ],
    ),
    (
        "The share of each trade",
        [
            "Stand-alone CVA: the trade as if it were alone. Incremental CVA: the CVA "
            "of the netting set minus its CVA without the trade — the price to charge "
            "for adding it, positive when the trade reduces the risk. Marginal CVA: "
            "the Euler allocation, the trade's value on the paths where the netting "
            "set is positive; the shares add up to the total. There is no exact "
            "allocation under a CSA with a threshold or a minimum transfer amount.",
        ],
    ),
    (
        "Where the paths are valued",
        [
            "Swaps and European swaptions have a closed-form value under the model. "
            "When the server has graphics cards, their paths are valued there: one "
            "GPU thread per path draws the random numbers the CPU would have drawn "
            "and applies the same formulas. The scenarios are identical and the "
            "values agree to about fourteen digits; the last ones differ because a "
            "graphics card does not compute an exponential exactly as a processor "
            "does. The result is the same, bit for bit, on one card or on two.",
            "A Bermudan or a scripted trade is valued by regression, and SIMM on "
            "every path needs the sensitivities of each scenario: both stay on the "
            "CPU. The response says where the paths ran and why.",
        ],
    ),
    (
        "Sources",
        [
            "Gregory, The xVA Challenge: Counterparty Risk, Funding, Collateral, "
            "Capital and Initial Margin, 4th ed., Wiley, 2020.",
            "Pykhtin & Zhu, A Guide to Modelling Counterparty Credit Risk, GARP Risk "
            "Review, 2007.",
            "Andersen, Pykhtin & Sokol, Rethinking the Margin Period of Risk, Journal "
            "of Credit Risk, 2017.",
            "Basel Committee on Banking Supervision, Basel Framework, CRE53 (internal "
            "models method: Effective EE, Effective EPE).",
        ],
    ),
]


def methodology() -> List[MethodologySection]:
    return [MethodologySection(title=t, paragraphs=p) for t, p in _SECTIONS]


def portfolios() -> List[Tuple[str, str, str]]:
    return [(pid, label, lesson) for pid, (label, lesson, _) in PORTFOLIOS.items()]


__all__ = [
    "compute",
    "market_inputs",
    "methodology",
    "portfolios",
    "PORTFOLIOS",
    "RATINGS",
    "XvaInputError",
    "XvaUnavailable",
]
