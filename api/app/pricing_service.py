import math
import re
import time
from datetime import date, timedelta
from typing import Dict, List, Optional, Tuple

import quantmodeling as qm

from .compute_timing import phase
from .schemas import (
    AmericanEngineType,
    AmericanVanillaRequest,
    AsianAverageType,
    AsianRequest,
    AutocallRequest,
    BarrierKind,
    BarrierRequest,
    BasketRequest,
    ComputeDevice,
    Greeks,
    RiskEntry,
    HullWhiteInputs,
    HullWhiteRatesFit,
    CommodityForwardRequest,
    CommodityOptionRequest,
    DatedAsianRequest,
    DigitalPayoffKind,
    DigitalRequest,
    DispersionSwapRequest,
    EngineType,
    FixedRateBondRequest,
    FutureRequest,
    FXForwardRequest,
    FXOptionRequest,
    LookbackExtremum,
    LookbackRequest,
    LookbackStyle,
    McRng,
    ModelChoice,
    ModelWarning,
    MountainRequest,
    PricingResponse,
    QuantoRequest,
    RainbowKind,
    RainbowRequest,
    ScriptRequest,
    ScriptValidateRequest,
    ScriptValidateResponse,
    ScriptedProductRequest,
    SwaptionVolFit,
    UnderlyingUsed,
    VanillaRequest,
    VarianceSwapRequest,
    VolatilitySwapRequest,
    ZeroCouponBondRequest,
)
from . import cancellation, precision
from .telemetry import tracer


def _multilevel(req, model: str) -> Dict:
    """price_script's multilevel and bias arguments for a script request
    (WP 26 §9.4, §9.5): MLMC by default under local vol, where it was
    measured fast, and under any model that refines its grid with
    mlmc="on"; to a share of the price unless an absolute target is given;
    the plain run's bias estimated either way. Within a route, a multilevel
    run stops when the route gives up (time_limit, issue #383): no check
    can stop a run already in the C++."""
    on = req.mlmc == "on" or (req.mlmc == "auto" and model == "local_vol")
    return dict(
        mlmc_eps=req.mlmc_eps if on else 0.0,
        mlmc_rel_eps=req.mlmc_rel_eps if on and req.mlmc_eps == 0.0 else 0.0,
        bias_estimate=req.bias_estimate,
        time_limit=cancellation.engine_time_limit(),
    )


#: Steps a year by default (ScriptPricingInputs.steps_per_year = None).
#: Heston prices by QE, nearly exact in law over a long step: 64 a year
#: (WP 26 §20). SLV keeps 252 although its variance is QE too: its leverage
#: is held over each step, and on SPY's 2026-11-19 fit (129 time columns,
#: dense at the short end) 64 a year put a 1y put 2 % above 504 a year, 128
#: a year 0.65 %, 252 within its noise. Local vol keeps 252 (WP 26 §9.3).
DEFAULT_STEPS_PER_YEAR = 252
HESTON_STEPS_PER_YEAR = 64
#: ...and at least this many over the product's life: at 64 a year a 0.1y
#: digital put has 6 steps and sits 0.75 % above 504 a year (WP 26 §20).
HESTON_MIN_STEPS = 32

_SCRIPT_DATE = re.compile(r"^\s*(\d{4}-\d{2}-\d{2})\b", re.M)


def _steps_per_year(req, model: str, script: str = "", valuation_date=None) -> int:
    if req.steps_per_year is not None:
        return req.steps_per_year
    if model != "heston":
        return DEFAULT_STEPS_PER_YEAR
    steps = HESTON_STEPS_PER_YEAR
    dates = [date.fromisoformat(d) for d in _SCRIPT_DATE.findall(script or "")]
    if dates and valuation_date is not None:
        life = (max(dates) - valuation_date).days / 365.0
        if life > 0:
            steps = max(steps, math.ceil(HESTON_MIN_STEPS / life))
    return min(steps, 504)


def _device(req, model: str) -> str:
    """Where a script request runs when it does not say: the GPU when the
    server has one ("auto") for a multilevel price, which the GPU returns
    several times faster within the same error (WP 26 §10), and for its
    AAD greeks under local vol, where they are multilevel too (§10.2), and
    for Heston and SLV, which the CPUs cannot price in time; the CPU for
    every other run. A request for second-order greeks goes to the
    GPU too: the bump on the adjoint runs only there, and on the CPU the
    box the user ticked would come back empty."""
    if req.device is not None:
        return req.device.value
    if getattr(req, "second_order", False) and req.greeks_method == "aad":
        return "auto"
    # Heston and SLV step a variance alongside the spot at 252 steps a year:
    # the page's default 200k paths took 17 s without greeks on the
    # production CPUs (two), over the 20 s timeout with them, against 0.4-2 s
    # on the GPU. "auto" stays on the CPU for what the GPU cannot run (a
    # model or a script without a compiled kernel, a server without a GPU)
    # and says so in the diagnostics; early exercise runs there since WP 26
    # §16.
    if model in ("heston", "slv"):
        return "auto"
    multilevel = _multilevel(req, model)["mlmc_eps"] > 0.0 or (
        _multilevel(req, model)["mlmc_rel_eps"] > 0.0
    )
    aad = req.greeks_method == "aad"
    return "auto" if multilevel and (not aad or model == "local_vol") else "cpu"


def _pricing_response_from_dict(result: Dict) -> PricingResponse:
    return PricingResponse(
        npv=result["npv"],
        greeks=result["greeks"],
        bond_analytics=result.get("bond_analytics"),
        diagnostics=result.get("diagnostics", ""),
        mc_std_error=result.get("mc_std_error", 0.0),
        device=result.get("device", "cpu"),
        gpus=result.get("gpus", 0),
        discretisation_bias=result.get("discretisation_bias"),
        discretisation_bias_std_error=result.get("discretisation_bias_std_error"),
        second_order_method=result.get("second_order_method"),
        # Only price_script(greeks_method="aad") populates this today
        # (blueprint/wp/17-aad.md §13.1); every other pricer's dict carries
        # risks=None, same as before this field existed.
        risks=result.get("risks"),
        # Only price_script populates this (scripting/model_advice.hpp).
        warnings=result.get("warnings", []),
    )


_DEVICES = {
    ComputeDevice.cpu: qm.ComputeDevice.Cpu,
    ComputeDevice.gpu: qm.ComputeDevice.Gpu,
    ComputeDevice.auto: qm.ComputeDevice.Auto,
}
_RNGS = {McRng.pcg32: qm.RngKind.Pcg32, McRng.philox: qm.RngKind.Philox}


_OPTION_TERMS = ("spot", "strike", "maturity", "rate", "dividend", "vol", "is_call")


def _set_option_terms(input_data, req) -> None:
    """Copies the terms every single-asset Black-Scholes input shares."""
    for name in _OPTION_TERMS:
        setattr(input_data, name, getattr(req, name))


def price_vanilla(req: VanillaRequest) -> PricingResponse:
    if req.is_american:
        # Price American vanilla
        input_data = qm.AmericanVanillaBSInput()
        _set_option_terms(input_data, req)
        input_data.tree_steps = req.tree_steps
        input_data.pde_space_steps = 100
        input_data.pde_time_steps = 100

        if req.engine == EngineType.trinomial:
            result = qm.price_american_vanilla_bs_trinomial(input_data)
        else:  # default to binomial for American
            result = qm.price_american_vanilla_bs_binomial(input_data)
    else:
        # Price European vanilla
        input_data = qm.VanillaBSInput()
        _set_option_terms(input_data, req)
        input_data.n_paths = req.n_paths
        input_data.seed = req.seed
        input_data.mc_epsilon = req.mc_epsilon
        input_data.device = _DEVICES[req.device]
        input_data.rng = _RNGS[req.rng]

        if req.engine == EngineType.pde:
            input_data.pde_space_steps = req.pde_space_steps
            input_data.pde_time_steps = req.pde_time_steps
            result = qm.price_vanilla_bs_pde(input_data)
        elif req.engine == EngineType.trinomial:
            input_data.tree_steps = req.tree_steps
            result = qm.price_vanilla_bs_trinomial(input_data)
        elif req.engine == EngineType.binomial:
            input_data.tree_steps = req.tree_steps
            result = qm.price_vanilla_bs_binomial(input_data)
        elif req.engine == EngineType.mc:
            result = qm.price_vanilla_bs_mc(input_data)
        else:
            result = qm.price_vanilla_bs_analytic(input_data)

    return _pricing_response_from_dict(result)


def price_american_vanilla(req: AmericanVanillaRequest) -> PricingResponse:
    input_data = qm.AmericanVanillaBSInput()
    _set_option_terms(input_data, req)
    input_data.tree_steps = req.tree_steps
    input_data.pde_space_steps = req.pde_space_steps
    input_data.pde_time_steps = req.pde_time_steps

    if req.engine == AmericanEngineType.mc:
        input_data.n_paths = req.n_paths
        input_data.seed = req.seed
        input_data.exercise_per_year = req.exercise_per_year
        input_data.threads = (
            0  # the available CPUs, at most 16: the exercise rule's pilot
        )
        result = qm.price_american_vanilla_bs_mc(input_data)
    elif req.engine == AmericanEngineType.trinomial:
        result = qm.price_american_vanilla_bs_trinomial(input_data)
    else:  # default to binomial
        result = qm.price_american_vanilla_bs_binomial(input_data)

    return _pricing_response_from_dict(result)


def price_asian(req: AsianRequest) -> PricingResponse:
    input_data = qm.AsianBSInput()
    _set_option_terms(input_data, req)
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_epsilon = req.mc_epsilon

    if req.average_type == AsianAverageType.geometric:
        input_data.average_type = qm.AsianAverageType.Geometric
    else:
        input_data.average_type = qm.AsianAverageType.Arithmetic

    if req.engine == EngineType.mc:
        result = qm.price_asian_bs_mc(input_data)
    else:
        result = qm.price_asian_bs_analytic(input_data)

    return _pricing_response_from_dict(result)


def price_dated_asian(req: DatedAsianRequest) -> PricingResponse:
    result = qm.price_dated_asian(
        req.spot,
        req.rate,
        req.dividend,
        req.vol,
        req.valuation_date.isoformat(),
        [d.isoformat() for d in req.fixing_dates],
        req.strike,
        req.is_call,
        req.geometric,
        req.day_count,
        req.n_paths,
        req.seed,
        req.sampler,
    )
    return _pricing_response_from_dict(result)


def validate_script(req: ScriptValidateRequest) -> ScriptValidateResponse:
    script, fixings = req.script, {}
    if req.start_today:
        # The fixing's level does not enter the analysis: one per asset.
        n = qm.validate_script(script, req.valuation_date.isoformat(), req.day_count)[
            "analysis"
        ]["n_underlyings"]
        script, fixings = _start_today(script, req.valuation_date, [100.0] * n)
    result = qm.validate_script(
        script,
        req.valuation_date.isoformat(),
        req.day_count,
        historical_fixings=fixings,
    )
    return ScriptValidateResponse(**result)


def _hard_threshold_greeks(
    req: ScriptRequest, valuation_date, model: str, script: str, fixings: Dict
) -> List[Dict]:
    """A warning when adjoint greeks are asked of a payoff that jumps at a
    level of the spot (a digital, a barrier) without smoothing: path by path
    the derivative of a step is zero, so the adjoint misses what the jump
    contributes to delta and vega -- it can read exactly zero. Only smoothing
    (`fuzzy`) gives the adjoint something to differentiate.

    A crisp comparison always jumps. The bridge survivals and lastStepDigital
    do not: the model computes them as smooth probabilities, except rough
    Bergomi, which has no closed form over a step and writes the threshold
    itself. A knock-out is smooth too (#369), unless priced as its threshold.

    `script` and `fixings`: the script as priced (a library product started
    today, its first date fixed), whose knock-out levels are then known."""
    if req.greeks_method != "aad" or req.fuzzy:
        return []
    analysis = qm.validate_script(
        script, valuation_date.isoformat(), req.day_count, historical_fixings=fixings
    )["analysis"]
    jumps = (
        analysis["spot_comparison"]
        or (model == "rough_bergomi" and analysis["spot_threshold_test"])
        or (req.knock_outs_as_thresholds and analysis["knock_out"])
    )
    if not jumps:
        return []
    return [
        {
            "code": "greeks_hard_threshold",
            "severity": "warning",
            "message": (
                "This payoff jumps when the spot crosses a level (a digital, a "
                "barrier), and the greeks are computed path by path: without "
                "smoothing they miss what the jump contributes, and can read "
                "exactly zero. Turn on Fuzzy to smooth the test and get "
                "meaningful greeks; the price itself is not affected."
            ),
        }
    ]


def _user_choice(model: str) -> Dict:
    return {
        "requested": model,
        "model": model,
        "code": "user",
        "reason": "Chosen by hand.",
    }


def _calibration_of(market, sv, model: str) -> Dict:
    """What was calibrated for `model`, for the response."""
    out: Dict = {"ticker": market.ticker, "snapshot": market.valuation_date}
    if sv is None:
        out["seconds"] = 0.0
        return out
    out["seconds"] = sv.seconds
    out["heston"] = {
        **sv.heston,
        "iv_rmse": sv.iv_rmse,
        "iv_worst": sv.iv_worst,
        "n_quotes": sv.n_quotes,
        "n_maturities": sv.n_maturities,
        "feller": sv.feller,
    }
    if model == "slv":
        out["leverage"] = {
            "min": sv.leverage_min,
            "max": sv.leverage_max,
            "clamped_share": sv.leverage_clamped_share,
            "clamped_mass": sv.leverage_clamped_mass,
            "n_particles": sv.n_particles,
        }
    return out


def _local_vol_markets(req: ScriptRequest, m) -> Tuple[Optional[list], str]:
    """One Dupire surface per underlying, or (None, why not): every
    underlying a ticker with a stored chain, in the payment currency (the
    quanto drift of a local vol would depend on the path), all on one
    snapshot."""
    from . import market_snapshot

    if any(a.fx is not None or a.drift_adjustment for a in m.assets):
        return None, (
            "an underlying is an exchange rate or is listed in another currency: "
            "its drift is the quanto one, which a local vol would make path-dependent"
        )
    markets = []
    for a in m.assets:
        try:
            markets.append(
                market_snapshot.local_vol_market(a.ticker, req.rate, req.valuation_date)
            )
        except market_snapshot.MarketDataUnavailable as exc:
            return None, f"{a.ticker} has no usable stored option chain ({exc})"
    snaps = {lv.valuation_date for lv in markets}
    if len(snaps) > 1:
        return None, (
            "the stored option chains are from different dates "
            f"({', '.join(sorted(d.isoformat() for d in snaps))})"
        )
    return markets, ""


def _start_today(script: str, valuation_date, spots: List[float]) -> Tuple[str, Dict]:
    """A library product's script started on the valuation date: its first
    date moved there (product_templates.start_on), fixed on today's spot(s)
    -- the levels in % of the initial level are then % of today's spot, a
    true European rather than a forward start."""
    from . import product_templates

    script = product_templates.start_on(script, valuation_date)
    return script, {valuation_date.isoformat(): [float(s) for s in spots]}


def _price_multi_asset(
    req: ScriptRequest, library: bool = False, start_today: bool = False
) -> PricingResponse:
    """A script reading spot(0), spot(1)...: each asset on the Dupire surface
    of its stored chain when every one has one (local vol, drivers
    correlated), else correlated Black-Scholes; inputs from the database
    (tickers) or typed."""
    from . import multi_asset_market

    parsed = qm.validate_script(
        req.script, req.valuation_date.isoformat(), req.day_count
    )
    n, given = parsed["analysis"]["n_underlyings"], len(req.underlyings)
    if n != given:
        raise ValueError(
            f"the script reads {n} underlying(s) (spot(0) to spot({n - 1})) "
            f"but {given} are given"
        )
    if n == 1 and req.model == "local_vol":
        raise ValueError(
            "model='local_vol' on one underlying takes `ticker`; `underlyings` "
            "with one entry is a quanto, priced under Black-Scholes"
        )
    horizon = parsed["events"][-1]["t"] if parsed["events"] else 0.0
    warnings: List[Dict] = []
    surfaces, no_surface = None, "the underlyings are typed"
    valuation_date = req.valuation_date
    if req.underlyings[0].spot is None:
        with phase("market"):
            m = multi_asset_market.multi_asset_market(
                [
                    (
                        u.ticker
                        if u.ticker is not None
                        else multi_asset_market.FxRate(u.fx)
                    )
                    for u in req.underlyings
                ],
                req.rate,
                req.valuation_date,
                horizon,
                req.currency,
            )
            if req.model in ("auto", "local_vol") and n > 1:
                surfaces, no_surface = _local_vol_markets(req, m)
        used = [
            UnderlyingUsed(
                ticker=a.ticker,
                fx=a.fx,
                currency=a.currency or None,
                spot=a.spot,
                dividend=a.dividend,
                vol=a.vol,
                vol_source=a.vol_source,
                drift_adjustment=a.drift_adjustment,
                drift_source=a.drift_source or None,
            )
            for a in m.assets
        ]
        corr, corr_source, warnings = m.correlation, m.correlation_source, m.warnings
    else:
        used = [
            UnderlyingUsed(
                spot=u.spot, dividend=u.dividend, vol=u.vol, vol_source="typed"
            )
            for u in req.underlyings
        ]
        corr, corr_source = req.correlation, "typed"
    if req.model == "local_vol" and surfaces is None:
        raise ValueError(f"model='local_vol' on several underlyings: {no_surface}")

    rec = qm.recommend_script_model(
        req.script,
        req.valuation_date.isoformat(),
        req.day_count,
        market_surface=surfaces is not None,
        stochastic=False,
    )
    choice = (
        {"requested": "auto", **rec} if req.model == "auto" else _user_choice(req.model)
    )
    model = choice["model"]
    if choice["code"] == "multi_asset" and n > 1 and no_surface:
        choice["reason"] += f" Here: {no_surface}."
    grids: Dict[str, list] = {}
    if model == "local_vol":
        # Each asset on its own surface, priced on the chains' market date
        # with the closes the surfaces were calibrated on.
        valuation_date = surfaces[0].valuation_date
        if valuation_date != req.valuation_date:
            warnings.append(
                {
                    "code": "market_date_shifted",
                    "severity": "info",
                    "message": f"Priced on the stored market date "
                    f"{valuation_date.isoformat()} (requested "
                    f"{req.valuation_date.isoformat()}): the latest option-chain "
                    "snapshot on or before it.",
                }
            )
        for u, lv in zip(used, surfaces):
            u.spot, u.dividend = lv.spot, lv.dividend
            u.vol_source = (
                f"local vol: Dupire surface of the {lv.valuation_date.isoformat()} "
                f"chain ({len(lv.K_grid)}x{len(lv.T_grid)}); the vol shown is the "
                "at-the-money implied vol"
            )
            warnings.extend(lv.warnings)
        grids = dict(
            K_grids=[lv.K_grid for lv in surfaces],
            T_grids=[lv.T_grid for lv in surfaces],
            sigma_loc_flats=[lv.sigma_loc_flat for lv in surfaces],
        )
    script, fixings = req.script, {}
    if start_today:
        script, fixings = _start_today(script, valuation_date, [u.spot for u in used])
    with (
        phase("engine"),
        tracer.start_as_current_span(
            "engine.price",
            attributes={
                "qm.product": "script",
                "qm.model": model,
                "qm.engine": "mc",
            },
        ),
    ):
        cancellation.check()  # after the market reads and calibrations
        result = qm.price_script(
            script,
            used[0].spot,
            req.rate,
            used[0].dividend + used[0].drift_adjustment,
            used[0].vol,
            valuation_date.isoformat(),
            req.day_count,
            req.fuzzy,
            req.default_eps,
            req.n_paths,
            req.seed,
            req.sampler,
            req.greeks_method,
            model if n > 1 else "black_scholes",
            [],
            [],
            [],
            _steps_per_year(req, model),
            spots=[u.spot for u in used],
            dividends=[u.dividend + u.drift_adjustment for u in used],
            vols=[u.vol for u in used],
            correlation=[x for row in corr for x in row],
            device=_device(req, model if n > 1 else "black_scholes"),
            rng=req.rng.value,
            control_variate=req.control_variate,
            antithetic=req.antithetic,
            importance_sampling=req.importance_sampling,
            milstein=req.milstein,
            second_order=req.second_order and req.greeks_method == "aad",
            terms_as_parameters=library,
            historical_fixings=fixings,
            knock_outs_as_thresholds=req.knock_outs_as_thresholds,
            **_multilevel(req, model if n > 1 else "black_scholes"),
            **grids,
        )
    result["warnings"] = (
        _hard_threshold_greeks(req, valuation_date, model, script, fixings)
        + warnings
        + list(result.get("warnings", []))
    )
    response = _pricing_response_from_dict(result)
    return response.model_copy(
        update={
            "model_choice": ModelChoice(
                **choice,
                underlyings=used,
                correlation=corr,
                correlation_source=corr_source,
            ),
            **({"script": script} if start_today else {}),
        }
    )


#: Library products with a closed form under Black-Scholes (engine='analytic').
ANALYTIC_SCRIPTED = (
    "european-call",
    "european-put",
    "digital-call",
    "digital-put",
    "equity-forward",
)


def _norm_cdf(x: float) -> float:
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def _norm_pdf(x: float) -> float:
    return math.exp(-0.5 * x * x) / math.sqrt(2.0 * math.pi)


def _black_scholes(
    product: str, terms: dict, S: float, r: float, q: float, sigma: float, T: float
):
    """Price and greeks of the library's vanilla products, the strike k of
    today's spot (they start today): Black-Scholes (Hull, ch. 15 and 26).
    Greeks per unit of spot, of volatility and of rate; theta per year."""
    k = float(terms["strike"])
    K = k * S
    sd = sigma * math.sqrt(T)
    d1 = (math.log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / sd
    d2 = d1 - sd
    dq, dr = math.exp(-q * T), math.exp(-r * T)
    if product == "equity-forward":
        n = float(terms["notional"])
        npv = n * (S * dq - K * dr)
        return (
            npv,
            dict(
                delta=n * dq,  # the strike K fixed in money
                gamma=0.0,
                vega=0.0,
                rho=n * K * T * dr,
                theta=n * (q * S * dq - r * K * dr),
            ),
            "forward",
        )
    if product.startswith("european"):
        call = product == "european-call"
        w = 1.0 if call else -1.0
        npv = w * (S * dq * _norm_cdf(w * d1) - K * dr * _norm_cdf(w * d2))
        theta = (
            -S * dq * _norm_pdf(d1) * sigma / (2 * math.sqrt(T))
            + w * q * S * dq * _norm_cdf(w * d1)
            - w * r * K * dr * _norm_cdf(w * d2)
        )
        return (
            npv,
            dict(
                delta=w * dq * _norm_cdf(w * d1),
                gamma=dq * _norm_pdf(d1) / (S * sd),
                vega=S * dq * _norm_pdf(d1) * math.sqrt(T),
                rho=w * K * T * dr * _norm_cdf(w * d2),
                theta=theta,
            ),
            ("European call" if call else "European put"),
        )
    call = product == "digital-call"
    w = 1.0 if call else -1.0
    P = float(terms["payout"])
    npv = P * dr * _norm_cdf(w * d2)
    dd2_dS = 1.0 / (S * sd)
    dd2_dsig = -d1 / sigma
    return (
        npv,
        dict(
            delta=w * P * dr * _norm_pdf(d2) * dd2_dS,
            gamma=-w * P * dr * _norm_pdf(d2) * (d2 + sd) / (S * S * sd * sd),
            vega=w * P * dr * _norm_pdf(d2) * dd2_dsig,
            rho=P
            * dr
            * (-T * _norm_cdf(w * d2) + w * _norm_pdf(d2) * math.sqrt(T) / sigma),
            theta=_digital_theta(w, P, S, K, r, q, sigma, T),
        ),
        ("cash-or-nothing digital call" if call else "cash-or-nothing digital put"),
    )


def _digital_theta(
    w: float, P: float, S: float, K: float, r, q, sigma, T: float
) -> float:
    """-dV/dT of the cash-or-nothing digital, by a central difference of its
    closed form in the maturity (an exact function: no Monte-Carlo noise)."""

    def v(t: float) -> float:
        sd = sigma * math.sqrt(t)
        d2 = (math.log(S / K) + (r - q - 0.5 * sigma * sigma) * t) / sd
        return P * math.exp(-r * t) * _norm_cdf(w * d2)

    h = min(1e-4, 0.5 * T)
    return -(v(T + h) - v(T - h)) / (2 * h)


def _price_scripted_analytic(req: ScriptedProductRequest) -> PricingResponse:
    """The closed form of a library product (ANALYTIC_SCRIPTED) under
    Black-Scholes on the ticker's stored market: spot and dividend of the
    snapshot, the at-the-money implied vol of its smile at the expiry. The
    product starts today (its strike k of today's spot), so these are the
    plain Black-Scholes formulas -- the oracle of its Monte-Carlo."""
    from types import SimpleNamespace

    from . import market_snapshot, product_templates

    if req.product not in ANALYTIC_SCRIPTED:
        raise ValueError(
            f"'{req.product}' has no closed form: engine='analytic' prices "
            f"{', '.join(ANALYTIC_SCRIPTED)}"
        )
    typed = not req.ticker
    if typed and (req.spot is None or req.vol is None):
        raise ValueError(
            "engine='analytic' reads its market from a `ticker`, or takes `spot` "
            "and `vol` (and `dividend`) typed"
        )
    with phase("market"):
        market = (
            None
            if typed
            else market_snapshot.local_vol_market(
                req.ticker, req.rate, req.valuation_date
            )
        )
    val = req.valuation_date if typed else market.valuation_date
    script = product_templates.render(
        req.product, req.terms, val, maturity=req.maturity, start=val
    )
    probe = product_templates.start_on(script, val + timedelta(days=1))
    events = qm.validate_script(probe, val.isoformat(), req.day_count)["events"]
    # the probe is the script a day later (its first date not yet past): T is
    # its last date from its first
    T = max(events[-1]["t"] - events[0]["t"], 1e-6)
    if typed:
        S, q, sigma = req.spot, req.dividend, req.vol
        source = "typed"
    else:
        S, q = market.spot, market.dividend
        with phase("calibration"):
            sigma = _atm_implied_vol(
                SimpleNamespace(script=script, day_count=req.day_count),
                market,
                val,
                True,
            )
        source = (
            f"the at-the-money implied vol of the {market.ticker} smile of "
            f"{val.isoformat()}"
        )
    defaults = {p.name: p.default for p in product_templates.get(req.product).params}
    terms = {**defaults, **req.terms}
    with phase("engine"):
        npv, greeks, what = _black_scholes(req.product, terms, S, req.rate, q, sigma, T)
    diagnostics = (
        f"Analytic {what}, Black-Scholes at vol {sigma:.4f} ({source}; strike "
        f"{float(terms['strike']):.4g} x spot {S:.4f}, expiry T = {T:.4f} y)"
    )
    response = _pricing_response_from_dict(
        {"npv": npv, "greeks": greeks, "diagnostics": diagnostics}
    )
    return response.model_copy(
        update={
            "script": script,
            "warnings": [] if typed else list(market.warnings),
        }
    )


def _price_rates_on_the_curve(req: ScriptedProductRequest) -> PricingResponse:
    """engine='analytic' for a rates product of the library: the script
    valued on the curve alone, the short rate's volatility set to (almost)
    zero -- its cash flows are then deterministic, each discounted by the
    curve, exactly the sum of the flows' present values. Only for payoffs
    linear in the curve (bonds, a swap's legs): an option on rates has no
    such value and keeps Monte-Carlo."""
    hw = (req.hull_white or HullWhiteInputs()).model_copy(update={"sigma": 1e-12})
    flat = req.model_copy(
        update={
            "engine": "mc",
            "hull_white": hw,
            "n_paths": 1_000,
            "greeks_method": "none",
            "second_order": False,
        }
    )
    response = price_scripted_product(flat)
    return response.model_copy(
        update={
            "mc_std_error": 0.0,
            "risks": None,
            "diagnostics": (
                "Analytic: the cash flows discounted on the "
                + (
                    f"flat curve at {req.rate:.4%} (typed)"
                    if hw.curve == "flat"
                    else f"{hw.currency} government curve"
                )
                + " (the script valued with no short-rate volatility: its flows "
                "are deterministic) | " + response.diagnostics
            ),
        }
    )


#: Library products on the SOFR curve with a closed form under the model
#: their Monte-Carlo simulates (single-curve Hull-White): the swap, its
#: value on the curve; the European swaption, Jamshidian's formula
#: generalised (engines/analytic/hull_white_swaption.hpp).
SOFR_CLOSED_FORM = ("interest-rate-swap", "european-swaption")

_DF_DATE = re.compile(r"df\((\d{4}-\d{2}-\d{2})\)")


def _swap_periods(script: str, valuation_date, day_count: str):
    """The fixed periods of a library swap or swaption, read off its script:
    the dates its df() reads, chained from the event that starts the swap
    (the last event before the first of them), on the script's own time
    axis (validate_script). Accruals are differences of those times: the
    script's yf() under an additive day count (ACT/365F, ACT/360)."""
    events = qm.validate_script(script, valuation_date.isoformat(), day_count)["events"]
    paid = sorted(set(_DF_DATE.findall(script)))
    if not paid:
        raise ValueError("the script reads no df(): not a swap")
    start = max((e for e in events if e["date"] < paid[0]), key=lambda e: e["t"])
    probe = "".join(f"{d}\n    x = 0\n" for d in paid)
    ends = [
        e["t"]
        for e in qm.validate_script(probe, valuation_date.isoformat(), day_count)[
            "events"
        ]
    ]
    starts = [start["t"]] + ends[:-1]
    return [(s0, e0, e0 - s0) for s0, e0 in zip(starts, ends)]


def _price_sofr_closed_form(req: ScriptedProductRequest) -> PricingResponse:
    """engine='analytic' for the swap and the European swaption of the
    library: the same model as their Monte-Carlo (Hull-White on the SOFR
    curve, calibrated to the traded swaptions), in closed form on the
    script's own dates. DV01 by moving every SOFR par quote 1bp and
    rebuilding the curve, (a, sigma) held; vega by moving sigma 1bp."""
    from . import product_templates, rates_derivatives

    start = time.perf_counter()
    template = product_templates.get(req.product)
    script = product_templates.render(
        req.product,
        req.terms,
        req.valuation_date,
        maturity=req.maturity,
        expiry=req.expiry,
    )
    terms = {p.name: float(req.terms.get(p.name, p.default)) for p in template.params}
    rate_name = "fixed_rate" if "fixed_rate" in terms else "strike"
    strike, side = terms[rate_name], terms["side"]
    notional = terms["notional"] * abs(side)
    periods = _swap_periods(script, req.valuation_date, req.day_count)
    swaption = req.product == "european-swaption"

    hw = req.hull_white or HullWhiteInputs(curve="sofr")
    flat = hw.curve == "flat"
    if flat:
        # every input typed: a flat curve at `rate`, a and sigma as given
        grid = [0.25 * i for i in range(1, 241)]

        def flat_curve(r: float):
            return grid, [math.exp(-r * t) for t in grid]

        times, dfs = flat_curve(req.rate)
        label, fit, m = f"flat curve at {req.rate:.4%} (typed)", None, None
        a, sigma = hw.mean_reversion, hw.sigma
    else:
        with phase("market"):
            _, times, dfs, label, fit = _sofr_inputs(hw)
            m = _sofr_market()
        a, sigma = fit.mean_reversion, fit.sigma

    def value(curve_times, curve_dfs, s: float) -> dict:
        return qm.hull_white_swap_on_dates(
            curve_times,
            curve_dfs,
            a,
            s,
            periods,
            strike,
            side > 0,
            notional,
            [periods[0][0]] if swaption else [],
        )

    key = "european" if swaption else "swap"
    with phase("engine"):
        base = value(times, dfs, sigma)
        npv = base[key]
        bumped = {}
        for h in (_BP, -_BP):
            if flat:
                t, d = flat_curve(req.rate + h)
            else:
                t, d = rates_derivatives.sofr_curve(
                    [(x, r + h) for x, r in m.swap_quotes]
                )
            bumped[h] = value(t, d, sigma)[key]
        dv01 = (bumped[_BP] - bumped[-_BP]) / 2
        gamma_bp = bumped[_BP] - 2 * npv + bumped[-_BP]
        vega = (
            (value(times, dfs, sigma + _BP)[key] - value(times, dfs, sigma - _BP)[key])
            / 2
            if swaption
            else 0.0
        )
    what = (
        f"European {'payer' if side > 0 else 'receiver'} swaption under Hull-White in "
        f"closed form (a = {a:.4f}, sigma = {sigma * 1e4:.1f}bp)"
        if swaption
        else f"{'Payer' if side > 0 else 'Receiver'} swap on the curve"
    )
    diagnostics = (
        f"Analytic: {what} on the {label} | forward swap rate "
        f"{base['forward']:.4%}, annuity {base['annuity']:.4f} | DV01 {dv01:,.2f}"
        + (
            " per bp (the flat rate +1bp)"
            if flat
            else " per bp (every SOFR par quote +1bp, the curve rebuilt)"
        )
        + (f", vega {vega:,.2f} per bp of sigma" if swaption else "")
    )
    choice = ModelChoice(
        requested="hull_white",
        model="hull_white",
        code="swaption_calibrated" if fit is not None and fit.calibrated else "user",
        reason=(
            f"Hull-White on the {label}: a = {a:.4f}, sigma = {sigma * 1e4:.1f}bp"
            + (
                f", fitting the vols to {fit.rmse_bp:.1f}bp on average "
                f"({fit.worst_bp:.1f}bp at worst)."
                if fit is not None
                else ", typed."
            )
        ),
        rates_calibration=fit,
    )
    warnings = []
    if fit is not None and not fit.converged:
        warnings.append(
            ModelWarning(
                code="hull_white_fit",
                severity="warning",
                message="The Hull-White calibration to the swaption vols did not converge.",
            )
        )
    return PricingResponse(
        npv=npv,
        greeks=Greeks(
            rho=dv01 / _BP,
            gamma=gamma_bp / _BP**2,
            vega=vega / _BP if swaption else None,
        ),
        bumped_greeks=["gamma", "rho", "vega"] if swaption else ["gamma", "rho"],
        diagnostics=diagnostics,
        mc_std_error=0.0,
        warnings=warnings,
        model_choice=choice,
        script=script,
        compute_ms=(time.perf_counter() - start) * 1e3,
        discretisation_bias=None,
    )


_BP = 1e-4


def price_scripted_product(req: ScriptedProductRequest) -> PricingResponse:
    """A library product from its term sheet (product_templates.py): the
    script is rendered with the terms, then priced like any script (or by
    its closed form, engine='analytic')."""
    from . import product_templates

    if req.precision is not None and req.engine != "analytic":
        return precision.price_to_precision(
            req, price_scripted_product, _market_quotes(req)
        )
    template = product_templates.get(req.product)
    if req.engine == "analytic":
        if req.product in SOFR_CLOSED_FORM:
            return _price_sofr_closed_form(req)
        if template.market == "rates":
            return _price_rates_on_the_curve(req)
        return _price_scripted_analytic(req)
    script = product_templates.render(
        req.product,
        req.terms,
        req.valuation_date,
        maturity=req.maturity,
        expiry=req.expiry,
    )
    fields = req.model_dump(
        exclude={"product", "terms", "engine", "maturity", "expiry"}
    )
    if req.hull_white is not None:
        # only what the request set: an unset sigma on the SOFR curve is the
        # calibrated one (_sofr_inputs)
        fields["hull_white"] = req.hull_white.model_dump(exclude_unset=True)
    # An equity product starts today, fixed on today's spot; a rates product
    # a few days ahead (its first coupon reads the curve, which a past date
    # cannot).
    if template.parts and template.market != "rates":
        parts = product_templates.render_parts(
            req.product,
            req.terms,
            req.valuation_date,
            maturity=req.maturity,
            expiry=req.expiry,
        )
        return _price_parts(req, template, script, parts, fields)
    response = _price_library_script(
        req, template, script, fields, start_today=template.market != "rates"
    )
    if template.market == "rates" and response.risks:
        response = _rates_risks(response)
    return response.model_copy(update={"script": response.script or script})


def _price_library_script(
    req, template, script: str, fields: dict, start_today: bool = False
) -> PricingResponse:
    """A library product by Monte-Carlo. One whose conditions jump at a level
    carries a measured smoothing width for its adjoint greeks (`# eps:`,
    WP 26 §5): without it, the derivative of a step is zero path by path
    and the greeks miss what the jump contributes. `# greeks: joint` prices
    and differentiates one smoothed run (its price bias measured below half
    an error); `separate` takes the price from an unsmoothed run and the
    greeks from a second, smoothed one. A request that sets `fuzzy` or
    `default_eps` itself is priced as it asks."""
    from . import product_templates

    eps = template.eps
    asked = {"fuzzy", "default_eps"} & req.model_fields_set
    if template.greeks == "knock-out":
        return _knock_out_library(
            req, template, script, fields, start_today, bool(asked)
        )
    if template.greeks == "last-step":
        if (
            req.model in product_templates.LAST_STEP_FALLBACK_MODELS
            and req.greeks_method == "aad"
            and not asked
        ):
            return _last_step_fallback(req, template, script, fields, start_today)
        eps = None  # lastStepDigital's own greeks
    if eps is None or req.greeks_method != "aad" or asked:
        return price_script(
            ScriptRequest(script=script, **fields),
            library=True,
            start_today=start_today,
        )
    smoothed = {**fields, "fuzzy": True, "default_eps": eps}
    if template.greeks == "joint":
        r = price_script(
            ScriptRequest(script=script, **smoothed),
            library=True,
            start_today=start_today,
        )
        return r.model_copy(
            update={
                "diagnostics": r.diagnostics
                + f" | price and greeks smoothed at eps {eps:g}, the width measured "
                "for this product (its price moves by less than half an error)"
            }
        )
    plain = {**fields, "greeks_method": "none", "second_order": False}
    priced = price_script(
        ScriptRequest(script=script, **plain), library=True, start_today=start_today
    )
    risks = price_script(
        ScriptRequest(script=script, **smoothed), library=True, start_today=start_today
    )
    return priced.model_copy(
        update={
            "greeks": risks.greeks,
            "risks": risks.risks,
            "bumped_greeks": risks.bumped_greeks,
            "second_order_method": risks.second_order_method,
            "compute_ms": (priced.compute_ms or 0.0) + (risks.compute_ms or 0.0),
            "warnings": priced.warnings
            + [w for w in risks.warnings if w not in priced.warnings],
            "diagnostics": priced.diagnostics
            + f" | greeks from a second run smoothed at eps {eps:g}, the width "
            "measured for this product: smoothing would move its price by more "
            "than half an error, so the price is the unsmoothed run's",
        }
    )


def _resolved_model(req, script: str, fields: dict, start_today: bool) -> str:
    """The model a library product is priced under: the request's, or what
    'auto' picks for its script on its market (price_script's choice, the
    stored market cached)."""
    return _resolved_choice(req, script, fields, start_today)["model"]


def _resolved_choice(req, script: str, fields: dict, start_today: bool) -> Dict:
    """_resolved_model with the reason: 'auto''s recommendation (model, code,
    reason), or the request's model chosen by hand."""
    from . import market_snapshot

    if req.model != "auto":
        return _user_choice(req.model)
    probe = ScriptRequest(script=script, **fields)
    market, valuation_date = None, probe.valuation_date
    if probe.ticker:
        market = market_snapshot.local_vol_market(
            probe.ticker, probe.rate, probe.valuation_date
        )
        valuation_date = market.valuation_date
    return {
        "requested": "auto",
        **_recommend(probe, valuation_date, market, True, start_today),
    }


def _with_greeks_of(priced: PricingResponse, risks: PricingResponse, note: str):
    """`priced`'s price with `risks`'s greeks: two runs of one product."""
    return priced.model_copy(
        update={
            "greeks": risks.greeks,
            "risks": risks.risks,
            "bumped_greeks": risks.bumped_greeks,
            "second_order_method": risks.second_order_method,
            "compute_ms": (priced.compute_ms or 0.0) + (risks.compute_ms or 0.0),
            "warnings": priced.warnings
            + [w for w in risks.warnings if w not in priced.warnings],
            "diagnostics": priced.diagnostics + " | " + note,
        }
    )


def _knock_out_library(
    req, template, script: str, fields: dict, start_today: bool, asked: bool
) -> PricingResponse:
    """A library product on knockOutAbove / Below (`# greeks: knock-out`,
    #369 L2). The model conditions its step into each date on survival:
    the price and its greeks, unbiased, with no smoothing. Except:

     - a model that cannot condition (KNOCK_OUT_THRESHOLD_MODELS): the hard
       threshold prices it and the greeks come from a second run smoothed
       at the product's measured eps -- the library's behaviour before;
     - Heston and SLV, whose conditioned greeks have infinite variance where
       Feller fails (#372): the price conditioned, the greeks smoothed."""
    from . import product_templates as pt

    model = _resolved_model(req, script, fields, start_today)
    thresholds = model in pt.KNOCK_OUT_THRESHOLD_MODELS
    smoothed_greeks = thresholds or model in pt.LAST_STEP_FALLBACK_MODELS
    if asked or req.greeks_method != "aad" or not smoothed_greeks:
        return price_script(
            ScriptRequest(script=script, **fields, knock_outs_as_thresholds=thresholds),
            library=True,
            start_today=start_today,
        )
    plain = {**fields, "greeks_method": "none", "second_order": False}
    priced = price_script(
        ScriptRequest(script=script, **plain, knock_outs_as_thresholds=thresholds),
        library=True,
        start_today=start_today,
    )
    smoothed = {
        **fields,
        "fuzzy": True,
        "default_eps": template.eps,
        "model": priced.model_choice.model if priced.model_choice else model,
    }
    risks = price_script(
        ScriptRequest(script=script, **smoothed, knock_outs_as_thresholds=True),
        library=True,
        start_today=start_today,
    )
    return _with_greeks_of(
        priced,
        risks,
        f"greeks from a second run on the thresholds smoothed at eps {template.eps:g}: "
        + (
            "this model cannot condition its step on a knock-out"
            if thresholds
            else "under this model the conditioned greeks have infinite variance "
            "where Feller fails"
        ),
    )


def _price_parts(req, template, script: str, parts, fields: dict) -> PricingResponse:
    """A library product of signed parts (Template.parts, #369 L2b): each
    part priced by Monte-Carlo as a library script -- a part on knock-outs
    as _knock_out_library prices one -- under the one model the product's
    script resolves to, on its own seed, and the price, the greeks and the
    risks summed with their signs, the errors in quadrature (the runs are
    independent)."""
    choice = _resolved_choice(req, script, fields, True)
    model = choice["model"]
    runs = []
    for k, (sign, part) in enumerate(parts):
        f = {**fields, "model": model, "seed": fields.get("seed", 1) + k}
        r = (
            _knock_out_library(
                req.model_copy(update={"model": model}), template, part, f, True, False
            )
            if "knockOut" in part
            else price_script(
                ScriptRequest(script=part, **f), library=True, start_today=True
            )
        )
        runs.append((sign, part, r))
    return _signed_sum(runs, choice if req.model == "auto" else None)


def _signed_sum(runs, choice) -> PricingResponse:
    """The signed sum of independent runs of one product's parts."""
    import math

    first = runs[0][2]
    npv = sum(s * r.npv for s, _, r in runs)
    err = math.sqrt(sum(r.mc_std_error**2 for _, _, r in runs))
    names = ("delta", "gamma", "vega", "theta", "rho", "vanna", "volga")
    g = {}
    for n in names:
        values = [getattr(r.greeks, n) for _, _, r in runs]
        g[n] = (
            None
            if any(v is None for v in values)
            else sum(s * v for (s, _, _), v in zip(runs, values))
        )
        ses = [getattr(r.greeks, n + "_std_error") for _, _, r in runs]
        g[n + "_std_error"] = (
            None
            if g[n] is None or any(e is None for e in ses)
            else math.sqrt(sum(e * e for e in ses))
        )
    risks = None
    if all(r.risks for _, _, r in runs):
        labels = [e.label for e in first.risks]
        if all([e.label for e in r.risks] == labels for _, _, r in runs):
            risks = [
                RiskEntry(
                    label=lab,
                    value=sum(s * r.risks[i].value for s, _, r in runs),
                    std_error=math.sqrt(
                        sum(r.risks[i].std_error ** 2 for _, _, r in runs)
                    ),
                )
                for i, lab in enumerate(labels)
            ]
    biases = [r.discretisation_bias for _, _, r in runs]
    warnings = []
    for _, _, r in runs:
        warnings += [w for w in r.warnings if w not in warnings]
    model_choice = first.model_choice
    knock_out_run = next((r for _, p, r in runs if "knockOut" in p), first)
    if knock_out_run.model_choice is not None:
        model_choice = knock_out_run.model_choice
        if choice is not None:  # 'auto': the product's choice, not "chosen by hand"
            model_choice = model_choice.model_copy(
                update={
                    k: choice[k] for k in ("requested", "code", "reason") if k in choice
                }
            )
    sign = lambda s: "+" if s > 0 else "-"  # noqa: E731
    return first.model_copy(
        update={
            "npv": npv,
            "mc_std_error": err,
            "greeks": Greeks(**g),
            "risks": risks,
            "bumped_greeks": [],
            "second_order_method": None,
            "warnings": warnings,
            "model_choice": model_choice,
            "compute_ms": sum((r.compute_ms or 0.0) for _, _, r in runs),
            "discretisation_bias": (
                None if any(b is None for b in biases) else sum(abs(b) for b in biases)
            ),
            "discretisation_bias_std_error": None,
            "script": "\n".join(f"# part: {sign(s)}\n{p}" for s, p, _ in runs),
            "diagnostics": f"signed parts ({len(runs)} runs, one model, the errors in quadrature) || "
            + " || ".join(f"part {sign(s)}: {r.diagnostics}" for s, _, r in runs),
        }
    )


def _last_step_fallback(
    req, template, script: str, fields: dict, start_today: bool
) -> PricingResponse:
    """A library digital on lastStepDigital under a model where its greeks
    fail (product_templates.LAST_STEP_FALLBACK_MODELS, #372): the price
    from lastStepDigital, unbiased, and the greeks from a second run on the
    threshold smoothed at the product's measured eps, as before #365."""
    from . import product_templates

    eps = template.eps
    plain = {**fields, "greeks_method": "none", "second_order": False}
    priced = price_script(
        ScriptRequest(script=script, **plain), library=True, start_today=start_today
    )
    smoothed = product_templates.smoothed_last_step(script, eps)
    risks = price_script(
        ScriptRequest(script=smoothed, **fields),
        library=True,
        start_today=start_today,
    )
    return priced.model_copy(
        update={
            "greeks": risks.greeks,
            "risks": risks.risks,
            "bumped_greeks": risks.bumped_greeks,
            "second_order_method": risks.second_order_method,
            "compute_ms": (priced.compute_ms or 0.0) + (risks.compute_ms or 0.0),
            "warnings": priced.warnings
            + [w for w in risks.warnings if w not in priced.warnings],
            "diagnostics": priced.diagnostics
            + f" | greeks from a second run on the threshold smoothed at eps {eps:g}: "
            "under this model lastStepDigital's own greeks have infinite variance "
            "(Heston, SLV) or read zero (rough Bergomi)",
        }
    )


#: The hybrid's stock, which a rates product never reads.
_EQUITY_RISKS = ("spot", "div", "vol")


def _rates_risks(response: PricingResponse) -> PricingResponse:
    """A rates product's adjoint risks: the curve's factors and the short
    rate's sigma, without the hybrid's unread stock; rho and vega their
    parallel and sigma risks, with their errors."""
    risks = [r for r in response.risks if r.label not in _EQUITY_RISKS]
    by = {r.label: r for r in risks}
    par, vol = by.get("curve_parallel"), by.get("rate_vol")
    greeks = response.greeks.model_copy(
        update={
            "delta": None,
            "delta_std_error": None,
            "rho": par.value if par else None,
            "rho_std_error": par.std_error if par else None,
            "vega": vol.value if vol else None,
            "vega_std_error": vol.std_error if vol else None,
        }
    )
    return response.model_copy(update={"risks": risks, "greeks": greeks})


def price_script(
    req: ScriptRequest, library: bool = False, start_today: bool = False
) -> PricingResponse:
    """Choose the model (req.model, or the one the script needs for 'auto'),
    calibrate what it needs from the database, then price.

    `library`: the script is a library product rendered from its term sheet
    (product_templates.py). Its numbers are then the product's terms, which
    the GPU's compiled kernel reads as data: one kernel per product shape,
    whatever the terms, and the kernel warm-up has it ready
    (kernel_warmup.py). A script a user wrote keeps its numbers as
    constants."""
    from . import market_snapshot, stochastic_vol

    if req.precision is not None:
        return precision.price_to_precision(
            req,
            lambda r: price_script(r, library=library, start_today=start_today),
            _market_quotes(req),
        )
    if req.underlyings is not None:
        return _price_multi_asset(req, library, start_today)

    market_warnings: List[Dict] = []
    valuation_date = req.valuation_date
    market = None
    if req.ticker and (
        req.model not in ("black_scholes", "hull_white") or req.spot is None
    ):
        # Market data comes from the database data-ingest fills, never from a
        # live source. A stored snapshot is a market date, and that date is
        # the valuation date the script is priced on.
        with phase("market"):
            market = market_snapshot.local_vol_market(
                req.ticker, req.rate, req.valuation_date
            )
        valuation_date = market.valuation_date
        market_warnings = list(market.warnings)

    model = req.model
    if model == "auto":
        choice = {
            "requested": "auto",
            **_recommend(req, valuation_date, market, True, start_today),
        }
    else:
        choice = _user_choice(model)

    sv = None
    if choice["model"] in ("heston", "slv"):
        try:
            with phase("calibration"):
                sv = stochastic_vol.calibrate(market)
        except stochastic_vol.CalibrationUnavailable as exc:
            if req.model != "auto":
                raise ValueError(
                    f"model='{req.model}' cannot be calibrated: {exc}"
                ) from exc
            # 'auto' falls back to the best model still available, and says so.
            choice = {
                "requested": "auto",
                **_recommend(req, valuation_date, market, False, start_today),
            }
            market_warnings.append(
                {
                    "code": "stochastic_calibration_failed",
                    "severity": "warning",
                    "message": f"Stochastic-vol calibration failed ({exc}); "
                    f"priced under {choice['model']} instead.",
                }
            )
    model = choice["model"]
    rv = None
    if model == "rough_bergomi":
        from . import rough_vol

        try:
            with phase("calibration"):
                rv = rough_vol.calibrate(market)
        except rough_vol.RoughCalibrationUnavailable as exc:
            raise ValueError(
                f"model='rough_bergomi' cannot be calibrated: {exc}"
            ) from exc
    if market is not None:
        choice["calibration"] = _calibration_of(market, sv, model)
        if rv is not None:
            choice["calibration"]["seconds"] = rv.seconds
            choice["calibration"]["rough_bergomi"] = {
                "H": rv.H,
                "eta": rv.eta,
                "rho": rv.rho,
                "xi_times": list(rv.xi_times),
                "xi_values": list(rv.xi_values),
                "xi_floored": rv.xi_floored,
                "iv_rmse": rv.iv_rmse,
                "iv_worst": rv.iv_worst,
                "n_quotes": rv.n_quotes,
                "n_maturities": rv.n_maturities,
                "mc_vol_error": rv.mc_vol_error,
            }

    if market is not None:
        spot, dividend, vol = market.spot, market.dividend, 0.0
        k_grid, t_grid = market.K_grid, market.T_grid
        if model in ("black_scholes", "hull_white"):
            with phase("calibration"):
                vol = _atm_implied_vol(req, market, valuation_date, start_today)
            choice["calibration"]["flat_vol"] = vol
    else:
        spot, dividend, vol = req.spot, req.dividend, req.vol
        k_grid, t_grid = [], []
    sigma = market.sigma_loc_flat if model == "local_vol" else []
    heston = sv.heston if sv is not None else {}
    hw_params, curve_times, curve_dfs = {}, [], []
    factor_basis, factor_labels = [], []
    if model == "hull_white":
        with phase("market"):
            hw_params, curve_times, curve_dfs, label, rates_fit = _hull_white_inputs(
                req
            )
        if req.greeks_method == "aad":
            factor_basis, factor_labels = _curve_factors(req, curve_times, curve_dfs)
        if rates_fit is not None and rates_fit.calibrated:
            choice["code"] = "swaption_calibrated"
            choice["reason"] = (
                f"Hull-White on the {label}: a = {hw_params['mean_reversion']:.4f}, "
                f"sigma = {hw_params['sigma'] * 1e4:.1f}bp, fitting the vols to "
                f"{rates_fit.rmse_bp:.1f}bp on average ({rates_fit.worst_bp:.1f}bp "
                "at worst)."
            )
        else:
            choice["reason"] = (
                f"Chosen by hand. Short rate: Hull-White (a = {hw_params['mean_reversion']:g}, "
                f"sigma = {hw_params['sigma']:g}, equity correlation {hw_params['rho']:g}) "
                f"fitted to the {label}."
            )
        if rates_fit is not None:
            choice["rates_calibration"] = rates_fit
        if market is None:
            choice["calibration"] = None
        else:
            choice["calibration"]["rates_curve"] = label
    leverage = list(sv.leverage_flat) if (sv is not None and model == "slv") else []
    # A precision target's vega bump (precision.py): every vol of the model
    # scaled by the same factor -- the local-vol grid, the SLV leverage (its
    # spot vol L sqrt(v)), a flat vol, Heston's v0 and theta by its square and
    # xi by it (the variance process of the scaled vol).
    scale = precision.VOL_SCALE.get()
    if scale != 1.0:
        sigma = [x * scale for x in sigma]
        leverage = [x * scale for x in leverage]
        vol = vol * scale if vol else vol
        if model == "heston" and heston:
            heston = {
                **heston,
                "v0": heston["v0"] * scale**2,
                "theta": heston["theta"] * scale**2,
                "xi": heston["xi"] * scale,
            }

    script, fixings = req.script, {}
    if start_today:
        script, fixings = _start_today(script, valuation_date, [spot])
    with (
        phase("engine"),
        tracer.start_as_current_span(
            "engine.price",
            attributes={"qm.product": "script", "qm.model": model, "qm.engine": "mc"},
        ),
    ):

        def run(greeks_method: str, second_order: bool, scheme: str, **over):
            cancellation.check()  # the QE price run, then the Euler risks run
            return qm.price_script(
                script,
                spot,
                req.rate,
                dividend,
                vol,
                valuation_date.isoformat(),
                req.day_count,
                req.fuzzy,
                req.default_eps,
                req.n_paths,
                req.seed,
                req.sampler,
                greeks_method,
                model,
                k_grid if model in ("local_vol", "slv") else [],
                (
                    list(sv.leverage_T_grid)
                    if model == "slv" and sv is not None
                    else t_grid if model == "local_vol" else []
                ),
                sigma,
                _steps_per_year(req, model, script, valuation_date),
                heston=heston,
                leverage_flat=leverage,
                device=_device(req, model),
                rng=req.rng.value,
                control_variate=req.control_variate,
                antithetic=req.antithetic,
                importance_sampling=req.importance_sampling,
                rough_bergomi=(
                    {"H": rv.H, "eta": rv.eta, "rho": rv.rho} if rv is not None else {}
                ),
                xi_times=list(rv.xi_times) if rv is not None else [],
                xi_values=list(rv.xi_values) if rv is not None else [],
                hull_white=hw_params,
                curve_times=curve_times,
                curve_dfs=curve_dfs,
                curve_factor_basis=factor_basis,
                curve_factor_labels=factor_labels,
                milstein=req.milstein,
                second_order=second_order,
                terms_as_parameters=library,
                historical_fixings=fixings,
                variance_scheme=scheme,
                knock_outs_as_thresholds=req.knock_outs_as_thresholds,
                **{**_multilevel(req, model), **over},
            )

        second = req.second_order and req.greeks_method == "aad"
        if model in ("heston", "slv"):
            result = _qe_price_euler_risks(run, req.greeks_method, second)
        else:
            result = run(req.greeks_method, second, "euler")
    result["warnings"] = (
        _hard_threshold_greeks(req, valuation_date, model, script, fixings)
        + market_warnings
        + list(result.get("warnings", []))
    )
    response = _pricing_response_from_dict(result)
    return response.model_copy(
        update={
            "model_choice": ModelChoice(**choice),
            **({"script": script} if start_today else {}),
        }
    )


def _market_quotes(req):
    """'market' precision's inputs, read when needed: the stored chain of the
    request's single ticker. None when there is none to read."""
    if (
        req.underlyings is not None
        or not req.ticker
        or req.model in ("black_scholes", "hull_white")
    ):
        return None
    from . import market_snapshot

    return lambda: market_snapshot.chain_quotes(
        req.ticker, req.rate, req.valuation_date
    )


def _qe_price_euler_risks(run, greeks_method: str, second_order: bool) -> Dict:
    """Heston and SLV (#317): the price run steps the variance by Andersen's
    QE, nearly unbiased at the page's steps where full-truncation Euler is
    not (SPY's fit, 2 kappa theta / xi^2 = 0.13: Euler +0.3 % on a 1y ATM
    call at 252 steps a year, QE within its noise at 32). The adjoint risks
    keep Euler, QE's branches having no pathwise derivative: with AAD greeks
    the price comes from a QE run and the greeks from a second, Euler run,
    and the diagnostics say so."""
    if greeks_method != "aad":
        return run(greeks_method, second_order, "qe")
    priced = run("none", False, "qe")
    risks = run("aad", second_order, "euler", bias_estimate=False)
    merged = dict(priced)
    for key in ("greeks", "risks", "second_order_method"):
        if key in risks:
            merged[key] = risks[key]
    merged["warnings"] = list(priced.get("warnings", [])) + [
        w for w in risks.get("warnings", []) if w not in priced.get("warnings", [])
    ]
    merged["diagnostics"] = (
        priced.get("diagnostics", "")
        + " | greeks from a second, adjoint run stepping the variance by Euler"
        + f" (its price {risks['npv']:.6g}); the price is the QE run's"
    )
    return merged


def _curve_factors(req, times, dfs) -> tuple:
    """The directions the tape differentiates a Hull-White curve in (the
    model's CurveFactors: pillar zero-rate moves per unit of each factor,
    row-major). On the SOFR curve, one factor per par swap rate it was
    bootstrapped from -- the bootstrap's Jacobian dz/dq, so each risk is to a
    quote (a bucketed DV01) -- and `curve_parallel`, every quote together.
    On a government curve, one per pillar's zero rate and `curve_parallel`,
    every zero rate together. Each comes with its own Monte-Carlo error."""
    hw = req.hull_white or HullWhiteInputs()
    if hw.curve == "sofr":
        from . import rates_derivatives

        jac, tenors = rates_derivatives.sofr_curve_jacobian(_sofr_market().swap_quotes)
        labels = [f"swap_rate[{t:g}]" for t in tenors]
        rows = [row + [sum(row)] for row in jac]
    else:
        labels = [f"zero[{t:g}]" for t in times]
        rows = [
            [1.0 if k == j else 0.0 for k in range(len(times))] + [1.0]
            for j in range(len(times))
        ]
    return [x for row in rows for x in row], labels + ["curve_parallel"]


def _hull_white_inputs(req) -> tuple:
    """model='hull_white': the parameters, and the discount curve the short
    rate is fitted to -- the currency's government curve from the database
    (the only free full term structure; rates.py). Missing curve: an error
    naming why, never a flat-rate stand-in."""
    from . import rates
    from .audit.payloads import MarketInputStatus
    from .valuation import record_market_input

    hw = req.hull_white or HullWhiteInputs()
    if hw.curve == "sofr":
        return _sofr_inputs(hw)
    if hw.curve == "flat":
        # every input typed: a flat curve at `rate` (the C++ model's own flat
        # curve when no pillar is given), a and sigma as the request says
        params = {"mean_reversion": hw.mean_reversion, "sigma": hw.sigma, "rho": hw.rho}
        return params, [], [], f"flat curve at {req.rate:.4%} (typed)", None
    try:
        curve = rates.currency_discount_curve(hw.currency)
    except rates.RatesUnavailable as exc:
        raise ValueError(
            f"model='hull_white' needs the {hw.currency} discount curve: {exc}"
        ) from exc
    record_market_input(
        f"curve:{hw.currency}",
        "db:macro.government_curve",
        curve.as_of.isoformat(),
        MarketInputStatus.OBSERVED,
        list(zip(curve.times, curve.dfs)),
    )
    params = {"mean_reversion": hw.mean_reversion, "sigma": hw.sigma, "rho": hw.rho}
    label = f"{hw.currency} government curve of {curve.as_of.isoformat()}"
    return params, list(curve.times), list(curve.dfs), label, None


def _sofr_market():
    from . import rates_derivatives

    try:
        return rates_derivatives.sofr_hull_white()
    except rates_derivatives.RatesMarketUnavailable as exc:
        raise ValueError(f"hull_white.curve='sofr' cannot be built: {exc}") from exc


def _sofr_inputs(hw: HullWhiteInputs) -> tuple:
    """hull_white.curve='sofr': the USD SOFR curve of the traded swaps, and
    Hull-White calibrated to the traded swaptions (rates_derivatives.
    sofr_hull_white) -- mean reversion and sigma the calibrated ones, unless
    the request set them (the Analytic engine of a swap sets sigma ~ 0)."""
    from .audit.payloads import MarketInputStatus
    from .valuation import record_market_input

    m = _sofr_market()
    record_market_input(
        "curve:USD-SOFR",
        "db:rates.dtcc_swap_rates",
        m.as_of.isoformat(),
        MarketInputStatus.OBSERVED,
        list(m.swap_quotes),
    )
    record_market_input(
        "swaption-vols:USD-SOFR",
        "db:rates.dtcc_swaptions",
        m.as_of.isoformat(),
        MarketInputStatus.OBSERVED,
        [list(v) for v in m.vols],
    )
    given = hw.model_fields_set
    a = hw.mean_reversion if "mean_reversion" in given else m.mean_reversion
    sigma = hw.sigma if "sigma" in given else m.sigma
    params = {"mean_reversion": a, "sigma": sigma, "rho": hw.rho}
    return params, list(m.times), list(m.dfs), m.label, _rates_fit(m, a, sigma, given)


def _rates_fit(m, a: float, sigma: float, given=frozenset()) -> HullWhiteRatesFit:
    return HullWhiteRatesFit(
        curve=m.label,
        as_of=m.as_of,
        window_start=m.window_start,
        mean_reversion=a,
        sigma=sigma,
        calibrated=not ({"mean_reversion", "sigma"} & set(given)),
        rmse_bp=m.rmse_bp,
        worst_bp=m.worst_bp,
        converged=m.converged,
        points=[
            SwaptionVolFit(expiry=x, tenor=t, market_vol=v, trades=n)
            for x, t, v, n in m.vols
        ],
        trades_used=m.trades_used,
        seconds=m.seconds,
    )


def _atm_implied_vol(req, market, valuation_date, start_today: bool = False) -> float:
    """Flat Black-Scholes on a ticker: the at-the-money implied vol of the
    stored SVI smile at the script's last event (sticky strike, K = spot).
    `start_today`: the script is a library product started on the valuation
    date -- its expiry is the last date's distance from the first."""
    from . import product_templates, vol_smile
    from .audit.payloads import MarketInputStatus
    from .valuation import record_market_input

    script = req.script
    if start_today:  # a day later, so that no date is past yet
        script = product_templates.start_on(script, valuation_date + timedelta(days=1))
    parsed = qm.validate_script(script, valuation_date.isoformat(), req.day_count)
    horizon = parsed["events"][-1]["t"] if parsed["events"] else 0.0
    if start_today and parsed["events"]:
        horizon -= parsed["events"][0]["t"]
    smile = vol_smile.Smile(
        ticker=market.ticker,
        snapshot=market.valuation_date,
        spot=market.spot,
        rate=market.rate,
        dividend=market.dividend,
        slices=tuple(market.surface_slices()),
        K_grid=(),
        T_grid=(),
        sigma_loc_flat=(),
    )
    vol = smile.implied_vol(market.spot, max(horizon, 1e-4))
    record_market_input(
        f"vol:{market.ticker}",
        "db:options.chain_snapshot",
        market.valuation_date.isoformat(),
        MarketInputStatus.OBSERVED,
        vol,
    )
    return vol


def _recommend(
    req: ScriptRequest,
    valuation_date,
    market,
    stochastic: bool,
    start_today: bool = False,
) -> Dict:
    """What 'auto' picks for the script as it is priced: a library product
    started today is read with its first date on the valuation date, fixed on
    today's spot (a strike struck there is a constant, so a European stays a
    European and is not mistaken for a forward start)."""
    script, fixings = req.script, {}
    if start_today:
        spot = market.spot if market is not None else (req.spot or 1.0)
        script, fixings = _start_today(script, valuation_date, [spot])
    return qm.recommend_script_model(
        script,
        valuation_date.isoformat(),
        req.day_count,
        market_surface=market is not None,
        stochastic=stochastic,
        historical_fixings=fixings,
    )


_BARRIER_KIND_MAP = {
    BarrierKind.up_and_in: qm.BarrierType.UpAndIn,
    BarrierKind.up_and_out: qm.BarrierType.UpAndOut,
    BarrierKind.down_and_in: qm.BarrierType.DownAndIn,
    BarrierKind.down_and_out: qm.BarrierType.DownAndOut,
}


def price_barrier(req: BarrierRequest) -> PricingResponse:
    input_data = qm.BarrierBSInput()
    _set_option_terms(input_data, req)
    input_data.barrier_level = req.barrier_level
    input_data.barrier_type = _BARRIER_KIND_MAP[req.barrier_kind]
    input_data.rebate = req.rebate
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_epsilon = req.mc_epsilon
    input_data.n_steps = req.n_steps
    input_data.brownian_bridge = req.brownian_bridge
    result = qm.price_barrier_bs_mc(input_data)
    return _pricing_response_from_dict(result)


_DIGITAL_PAYOFF_MAP = {
    DigitalPayoffKind.cash_or_nothing: qm.DigitalPayoffType.CashOrNothing,
    DigitalPayoffKind.asset_or_nothing: qm.DigitalPayoffType.AssetOrNothing,
}


def price_digital(req: DigitalRequest) -> PricingResponse:
    input_data = qm.DigitalBSInput()
    _set_option_terms(input_data, req)
    input_data.payoff_type = _DIGITAL_PAYOFF_MAP[req.payoff_type]
    input_data.cash_amount = req.cash_amount
    result = qm.price_digital_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


_LOOKBACK_STYLE_MAP = {
    LookbackStyle.fixed_strike: qm.LookbackStyle.FixedStrike,
    LookbackStyle.floating_strike: qm.LookbackStyle.FloatingStrike,
}

_LOOKBACK_EXTREMUM_MAP = {
    LookbackExtremum.minimum: qm.LookbackExtremum.Minimum,
    LookbackExtremum.maximum: qm.LookbackExtremum.Maximum,
}


def price_lookback(req: LookbackRequest) -> PricingResponse:
    input_data = qm.LookbackBSInput()
    _set_option_terms(input_data, req)
    input_data.style = _LOOKBACK_STYLE_MAP[req.style]
    input_data.extremum = _LOOKBACK_EXTREMUM_MAP[req.extremum]
    input_data.n_steps = req.n_steps
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_antithetic = req.mc_antithetic
    input_data.mc_epsilon = req.mc_epsilon
    result = qm.price_lookback_bs_mc(input_data)
    return _pricing_response_from_dict(result)


def price_basket(req: BasketRequest) -> PricingResponse:
    n = len(req.spots)
    # Per-asset dividends: use provided list or replicate a single 0.0
    dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    # Per-asset weights: use provided list or equal-weight
    weights = list(req.weights) if len(req.weights) == n else [1.0 / n] * n
    # Build full n×n correlation matrix from pairwise scalar
    rho = req.pairwise_correlation
    correlations = [[1.0 if i == j else rho for j in range(n)] for i in range(n)]

    input_data = qm.BasketBSInput()
    input_data.spots = req.spots
    input_data.vols = req.vols
    input_data.dividends = dividends
    input_data.weights = weights
    input_data.correlations = correlations
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.is_call = req.is_call
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_antithetic = req.mc_antithetic

    result = qm.price_basket_bs_mc(input_data)
    return _pricing_response_from_dict(result)


def price_future(req: FutureRequest) -> PricingResponse:
    input_data = qm.EquityFutureInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.notional = req.notional

    result = qm.price_future_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


def price_zero_coupon_bond(req: ZeroCouponBondRequest) -> PricingResponse:
    input_data = qm.ZeroCouponBondInput()
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.discount_times = req.discount_times
    input_data.discount_factors = req.discount_factors

    result = qm.price_zero_coupon_bond_analytic(input_data)
    return _pricing_response_from_dict(result)


def price_fixed_rate_bond(req: FixedRateBondRequest) -> PricingResponse:
    input_data = qm.FixedRateBondInput()
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.coupon_rate = req.coupon_rate
    input_data.coupon_frequency = req.coupon_frequency
    input_data.notional = req.notional
    input_data.discount_times = req.discount_times
    input_data.discount_factors = req.discount_factors

    result = qm.price_fixed_rate_bond_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Autocall
# ---------------------------------------------------------------------------


def price_autocall(req: AutocallRequest) -> PricingResponse:
    input_data = qm.AutocallBSInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.observation_dates = req.observation_dates
    input_data.autocall_barrier = req.autocall_barrier
    input_data.coupon_barrier = req.coupon_barrier
    input_data.put_barrier = req.put_barrier
    input_data.coupon_rate = req.coupon_rate
    input_data.notional = req.notional
    input_data.memory_coupon = req.memory_coupon
    input_data.ki_continuous = req.ki_continuous
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_autocall_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Mountain (Himalaya)
# ---------------------------------------------------------------------------


def _build_corr_matrix(n: int, corrs: List[List[float]]) -> List[List[float]]:
    """Return provided correlation matrix or identity if empty."""
    if corrs and len(corrs) == n:
        return corrs
    return [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]


def price_mountain(req: MountainRequest) -> PricingResponse:
    n = len(req.spots)
    input_data = qm.MountainBSInput()
    input_data.spots = req.spots
    input_data.vols = req.vols
    input_data.dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    input_data.correlations = _build_corr_matrix(n, req.correlations)
    input_data.observation_dates = req.observation_dates
    input_data.strike = req.strike
    input_data.is_call = req.is_call
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_mountain_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Variance Swap
# ---------------------------------------------------------------------------


def price_variance_swap(req: VarianceSwapRequest) -> PricingResponse:
    input_data = qm.VarianceSwapBSInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.maturity = req.maturity
    input_data.strike_var = req.strike_var
    input_data.notional = req.notional
    input_data.observation_dates = req.observation_dates
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    if req.engine == EngineType.mc:
        result = qm.price_variance_swap_bs_mc(input_data)
    else:
        result = qm.price_variance_swap_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Volatility Swap
# ---------------------------------------------------------------------------


def price_volatility_swap(req: VolatilitySwapRequest) -> PricingResponse:
    input_data = qm.VolatilitySwapBSInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.maturity = req.maturity
    input_data.strike_vol = req.strike_vol
    input_data.notional = req.notional
    input_data.observation_dates = req.observation_dates
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_volatility_swap_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Dispersion Swap
# ---------------------------------------------------------------------------


def price_dispersion_swap(req: DispersionSwapRequest) -> PricingResponse:
    n = len(req.spots)
    dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    weights = list(req.weights) if len(req.weights) == n else [1.0 / n] * n
    rho = req.pairwise_correlation
    correlations = [[1.0 if i == j else rho for j in range(n)] for i in range(n)]

    input_data = qm.DispersionBSInput()
    input_data.spots = req.spots
    input_data.vols = req.vols
    input_data.dividends = dividends
    input_data.weights = weights
    input_data.correlations = correlations
    input_data.maturity = req.maturity
    input_data.strike_spread = req.strike_spread
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.observation_dates = req.observation_dates
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_dispersion_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# FX Forward
# ---------------------------------------------------------------------------


def price_fx_forward(req: FXForwardRequest) -> PricingResponse:
    input_data = qm.FXForwardInput()
    input_data.spot = req.spot
    input_data.rate_domestic = req.rate_domestic
    input_data.rate_foreign = req.rate_foreign
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.notional = req.notional
    result = qm.price_fx_forward_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# FX Option
# ---------------------------------------------------------------------------


def price_quanto(req: QuantoRequest) -> PricingResponse:
    input_data = qm.QuantoBSInput()
    for field in (
        "spot", "strike", "maturity", "rate_domestic", "rate_foreign", "dividend",
        "vol", "fx_vol", "correlation", "fx_rate", "is_call", "n_paths", "seed",
    ):  # fmt: skip
        setattr(input_data, field, getattr(req, field))
    if req.engine == "mc":
        result = qm.price_quanto_bs_mc(input_data)
    else:
        result = qm.price_quanto_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


def price_fx_option(req: FXOptionRequest) -> PricingResponse:
    input_data = qm.FXOptionInput()
    input_data.spot = req.spot
    input_data.rate_domestic = req.rate_domestic
    input_data.rate_foreign = req.rate_foreign
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.is_call = req.is_call
    input_data.notional = req.notional
    result = qm.price_fx_option_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Commodity Forward
# ---------------------------------------------------------------------------


def price_commodity_forward(req: CommodityForwardRequest) -> PricingResponse:
    input_data = qm.CommodityForwardInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.storage_cost = req.storage_cost
    input_data.convenience_yield = req.convenience_yield
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.notional = req.notional
    result = qm.price_commodity_forward_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Commodity Option
# ---------------------------------------------------------------------------


def price_commodity_option(req: CommodityOptionRequest) -> PricingResponse:
    input_data = qm.CommodityOptionInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.storage_cost = req.storage_cost
    input_data.convenience_yield = req.convenience_yield
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.is_call = req.is_call
    input_data.notional = req.notional
    result = qm.price_commodity_option_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Rainbow (worst-of / best-of)
# ---------------------------------------------------------------------------


def price_rainbow(req: RainbowRequest) -> PricingResponse:
    n = len(req.spots)
    dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    rho = req.pairwise_correlation
    correlations = [[1.0 if i == j else rho for j in range(n)] for i in range(n)]

    input_data = qm.RainbowBSInput()
    input_data.spots = req.spots
    input_data.vols = req.vols
    input_data.dividends = dividends
    input_data.correlations = correlations
    input_data.maturity = req.maturity
    input_data.strike = req.strike
    input_data.is_call = req.is_call
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed

    if req.rainbow_kind == RainbowKind.best_of:
        result = qm.price_best_of_bs_mc(input_data)
    else:
        result = qm.price_worst_of_bs_mc(input_data)
    return _pricing_response_from_dict(result)
