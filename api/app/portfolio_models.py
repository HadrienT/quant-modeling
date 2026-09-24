"""Which model marks each derivative of a portfolio — the choice an equity
derivatives desk makes — and the record of that choice shown on the
position.

The market inputs (spot, time to expiry, rate curves, dividend) are set by
portfolio_valuation._mark_derivative; this module adds the volatility and
runs the model. Everything rests on the day's smile when one exists
(vol_smile.py: the SVI surface fitted to the stored option chain) and says
so when it does not:

| Product | With a smile | Without one |
|---|---|---|
| European, American vanilla | Black-Scholes at the smile's implied vol for that strike and maturity (closed form; CRR tree for early exercise) — a vanilla is marked, not modelled | same, at the realised-vol proxy |
| Digital | Black-Scholes at the strike's implied vol, minus vega × skew: the limit of a tight call spread on the smile, which a flat-vol digital misprices by the whole skew term | flat Black-Scholes |
| Asian, barrier, lookback | Dupire local volatility — reprices every vanilla of the surface, the standard base for path-dependent equity products — by Monte-Carlo on the product's script (Savine), past fixings replayed | same script, Black-Scholes at the realised-vol proxy |
| Quanto | Black-Scholes with the quanto drift adjustment, asset vol from the smile, FX vol and asset/FX correlation estimated from history (no FX options stored) | same, asset vol proxied |
| Future | cost of carry, no volatility | — |

Not used, and why: Heston and rough Bergomi exist in the C++ core but are
neither exposed to Python nor calibrated to stored data, and a stochastic-
volatility model that does not reprice the day's vanillas would mark a book
off its own hedges. Stochastic-local volatility (the SLV simulator, also in
the core) is the full desk standard for barriers; it needs a calibrated
leverage function, which the pipeline does not produce yet.
"""

from __future__ import annotations

import math
from dataclasses import replace
from datetime import date
from functools import lru_cache
from typing import TYPE_CHECKING, Callable, Dict, List, Optional, Tuple

import quantmodeling as qm

from . import scripted_payoffs, valuation, vol_smile
from .portfolio_marks import Input, Mark, ModelInfo, ModelParam
from .portfolio_schemas import DerivativeSpec

if TYPE_CHECKING:  # pragma: no cover
    from .portfolio_valuation import MarketData

#: Monte-Carlo of scripted products: paths, and one seed for every day, so
#: that consecutive marks share their draws (common random numbers) and the
#: daily P&L is not simulation noise.
SCRIPT_PATHS = 20_000
SCRIPT_SEED = 7
#: Relative spot bump of the finite-difference delta and gamma.
BUMP = 0.01
SCRIPTED = frozenset(scripted_payoffs.BUILDERS)


def _rate(params: dict) -> float:
    return float(params.get("rate", params.get("rate_domestic", 0.0)))


def _asset_vol(
    spec: DerivativeSpec, params: dict, d: date, md: "MarketData", K: float, T: float
) -> Tuple[float, ModelParam, Input, Optional[vol_smile.Smile]]:
    """The volatility the model runs at, from the best source available."""
    u = spec.underlying
    fallback = _rate(params)

    def one_year(snap: date) -> float:
        got = md.zero_rate(spec.currency, snap, 1.0)
        return got[1] if got else fallback

    smile = vol_smile.smile_for(u, d, one_year)
    if smile is not None:
        s = smile.implied_vol(K, T)
        src = f"SVI surface of the {u} option chain of {smile.snapshot}"
        return (
            s,
            ModelParam("implied vol at K, T", s, status="calibrated", source=src),
            Input(f"vol {u} (implied, smile)", "observed", s, smile.snapshot),
            smile,
        )
    got = md.realised_vol(u, d) if u else None
    if got is not None:
        src = f"{u} realised vol, 63 business days to {got[0]}: no option chain stored"
        return (
            got[1],
            ModelParam("volatility", got[1], status="proxied", source=src),
            Input(f"vol {u} (63-day realised, proxy)", "proxied", got[1], got[0]),
            None,
        )
    v = float(params.get("vol", 0.2))
    return (
        v,
        ModelParam("volatility", v, status="default", source="typed with the trade"),
        Input("vol", "default", v),
        None,
    )


def _market_params(params: dict, inputs: List[Input]) -> List[ModelParam]:
    """The inputs portfolio_valuation already set, as model parameters."""
    status = {i.name.split(" ")[0]: i for i in inputs}
    out = []
    for key, label in (
        ("spot", "spot"),
        ("maturity", "time to expiry (y)"),
        ("rate", "rate"),
        ("rate_domestic", "domestic rate"),
        ("rate_foreign", "foreign rate"),
        ("dividend", "dividend yield"),
    ):
        if key not in params:
            continue
        i = status.get("time" if key == "maturity" else key)
        out.append(
            ModelParam(
                label,
                float(params[key]),
                status=i.status if i else "default",
                source=(
                    f"{i.name}, {i.as_of}" if i and i.as_of else "typed with the trade"
                ),
            )
        )
    if "strike" in params:
        out.insert(1, ModelParam("strike", float(params["strike"]), status="contract"))
    return out


def _greeks(g) -> Dict[str, Optional[float]]:
    return {
        "delta": g.delta,
        "gamma": g.gamma,
        "vega": g.vega,
        "theta": g.theta,
        "rho": g.rho,
    }


def _catalog(product: str, params: dict):
    req = valuation.PRODUCTS[product].request.model_validate(params)
    return valuation.price(product, req).response


# ── Vanillas ─────────────────────────────────────────────────────────────────


def _vanilla(spec, d, md, params, inputs) -> Mark:
    K, T = float(params["strike"]), float(params["maturity"])
    sigma, vparam, vinput, smile = _asset_vol(spec, params, d, md, K, T)
    params["vol"] = sigma
    american = spec.product == "american_vanilla"
    resp = _catalog(spec.product, params)
    engine = (
        f"CRR binomial tree, {params.get('tree_steps', 100)} steps"
        if american
        else "Black-Scholes closed form"
    )
    why = (
        "A vanilla is marked at its implied volatility: Black-Scholes is only the "
        "quoting convention, the vol is read at this strike and maturity on the "
        "day's smile."
        if smile
        else "No option chain is stored for this underlying on this date, so the "
        "smile is unknown: Black-Scholes at the realised-volatility proxy."
    )
    return Mark(
        float(resp.npv),
        spec.currency,
        inputs + [vinput],
        _greeks(resp.greeks),
        model=ModelInfo(
            (
                "Black-Scholes on the SVI smile"
                if smile
                else "Black-Scholes, flat volatility"
            ),
            engine,
            why,
            _market_params(params, inputs) + [vparam],
        ),
    )


def _digital(spec, d, md, params, inputs) -> Mark:
    K, T = float(params["strike"]), float(params["maturity"])
    sigma, vparam, vinput, smile = _asset_vol(spec, params, d, md, K, T)
    params["vol"] = sigma
    resp = _catalog("digital", params)
    value = float(resp.npv)
    extra: List[ModelParam] = []
    if smile is not None:
        # d/dK of the vanilla on the smile: the flat-vol digital plus the
        # skew term, −vega·∂σ/∂K for a call (+ for a put), scaled by the cash
        # amount (cash-or-nothing) or the strike (asset-or-nothing).
        skew = smile.dvol_dK(K, T)
        vanilla = {
            k: params[k]
            for k in (
                "spot",
                "strike",
                "maturity",
                "rate",
                "dividend",
                "vol",
                "is_call",
            )
        }
        vega = float(
            _catalog("vanilla", {**vanilla, "engine": "analytic"}).greeks.vega or 0.0
        )
        cash = params.get("payoff_type", "cash-or-nothing") == "cash-or-nothing"
        scale = float(params.get("cash_amount", 1.0)) if cash else K
        sign = -1.0 if params.get("is_call", True) else 1.0
        value += sign * scale * vega * skew
        extra = [
            ModelParam("skew ∂σ/∂K", skew, status="calibrated", source=vparam.source),
            ModelParam(
                "skew correction", sign * scale * vega * skew, status="calibrated"
            ),
        ]
    why = (
        "A digital is the limit of a tight call spread, so its price is the strike "
        "derivative of the vanilla on the smile: the flat-vol digital at the "
        "strike's implied vol, corrected by vega × skew."
        if smile
        else "No option chain stored on this date: flat Black-Scholes digital at the "
        "realised-volatility proxy, without the skew correction a smile would add."
    )
    return Mark(
        value,
        spec.currency,
        inputs + [vinput],
        _greeks(resp.greeks),
        model=ModelInfo(
            (
                "Smile-consistent digital (call-spread replication)"
                if smile
                else "Black-Scholes digital, flat volatility"
            ),
            "closed form",
            why,
            _market_params(params, inputs) + [vparam] + extra,
        ),
    )


def _quanto(spec, d, md, params, inputs) -> Mark:
    K, T = float(params["strike"]), float(params["maturity"])
    sigma, vparam, vinput, smile = _asset_vol(spec, params, d, md, K, T)
    params["vol"] = sigma
    u = spec.underlying
    u_ccy = md.currency(u) if u else spec.currency
    extra = []
    fxv = md.fx_realised_vol(u_ccy, spec.currency, d)
    if fxv is not None:
        params["fx_vol"] = fxv[1]
        extra.append(
            ModelParam(
                "FX vol",
                fxv[1],
                status="proxied",
                source=f"{u_ccy}{spec.currency} ECB rates, realised over 63 business days to {fxv[0]}: no FX options stored",
            )
        )
    else:
        extra.append(
            ModelParam(
                "FX vol",
                float(params.get("fx_vol", 0.0)),
                status="default",
                source="typed with the trade",
            )
        )
    rho = md.asset_fx_correlation(u, u_ccy, spec.currency, d) if u else None
    if rho is not None:
        params["correlation"] = rho[1]
        extra.append(
            ModelParam(
                "asset/FX correlation",
                rho[1],
                status="proxied",
                source=f"daily log returns over the year to {rho[0]}",
            )
        )
    else:
        extra.append(
            ModelParam(
                "asset/FX correlation",
                float(params.get("correlation", 0.0)),
                status="default",
                source="typed with the trade",
            )
        )
    extra.append(
        ModelParam(
            "fixed conversion rate",
            float(params.get("fx_rate", 1.0)),
            status="contract",
        )
    )
    resp = _catalog("quanto", {**params, "engine": "analytic"})
    return Mark(
        float(resp.npv),
        spec.currency,
        inputs + [vinput],
        _greeks(resp.greeks),
        model=ModelInfo(
            "Black-Scholes with quanto drift adjustment",
            "closed form",
            "A quanto pays a foreign asset's return at a fixed rate: its drift carries "
            "−ρ·σ_S·σ_X. The asset vol is read on the smile where one is stored; "
            "FX vol and correlation are historical estimates (no FX options stored).",
            _market_params(params, inputs) + [vparam] + extra,
        ),
    )


def _future(spec, d, md, params, inputs) -> Mark:
    resp = _catalog("future", params)
    return Mark(
        float(resp.npv),
        spec.currency,
        inputs,
        _greeks(resp.greeks),
        model=ModelInfo(
            "Cost of carry",
            "closed form",
            "A future has no optionality: F = S·e^{(r−q)T}, no volatility enters.",
            _market_params(params, inputs),
        ),
    )


# ── Path-dependent products: scripts ─────────────────────────────────────────


@lru_cache(maxsize=256)
def _terms(product: str, frozen: tuple, start: date, expiry: date, ccy: str):
    return scripted_payoffs.terms(product, dict(frozen), start, expiry, ccy)


def _frozen(params: dict) -> tuple:
    keep = (
        "strike",
        "is_call",
        "average_type",
        "style",
        "extremum",
        "barrier_level",
        "barrier_kind",
        "rebate",
    )
    return tuple(sorted((k, params[k]) for k in keep if k in params))


def _scripted(spec, d, md, params, inputs, today: date) -> Mark:
    u = spec.underlying
    start_raw = params.get("start_date")
    if not u or not start_raw:
        return Mark(
            None,
            spec.currency,
            inputs,
            note="a path-dependent product needs its underlying and its start date",
        )
    start = date.fromisoformat(str(start_raw))
    t = _terms(spec.product, _frozen(params), start, spec.expiry, spec.currency)
    closes = md.closes_from(u, start)
    fixings: Dict[str, List[float]] = {}
    done: List[Tuple[date, float]] = []
    stale = 0
    for fd in t.dates:
        if fd > d:
            break
        got = closes.at(fd)
        if got is None:
            return Mark(
                None,
                spec.currency,
                inputs,
                note=f"no stored close for {u} on or before the fixing of {fd}",
            )
        stale += got[0] != fd
        fixings[fd.isoformat()] = [float(got[1])]
        done.append((fd, float(got[1])))
    S, r, q = float(params["spot"]), float(params["rate"]), float(params["dividend"])
    K = float(params.get("strike", S))
    T = max((spec.expiry - d).days / 365.25, 1e-6)
    sigma, vparam, vinput, smile = _asset_vol(spec, params, d, md, K, T)

    def run(spot: float) -> dict:
        common = dict(
            n_paths=SCRIPT_PATHS, seed=SCRIPT_SEED, historical_fixings=fixings
        )
        if smile is not None:
            return qm.price_script(
                t.script,
                spot,
                r,
                q,
                0.0,
                d.isoformat(),
                model="local_vol",
                K_grid=list(smile.K_grid),
                T_grid=list(smile.T_grid),
                sigma_loc_flat=list(smile.sigma_loc_flat),
                steps_per_year=252,
                **common,
            )
        return qm.price_script(t.script, spot, r, q, sigma, d.isoformat(), **common)

    try:
        res = run(S)
        greeks: Dict[str, Optional[float]] = {}
        if d >= today:  # finite differences on the common draws, today only
            up, dn = run(S * (1 + BUMP))["npv"], run(S * (1 - BUMP))["npv"]
            h = S * BUMP
            greeks = {
                "delta": (up - dn) / (2 * h),
                "gamma": (up - 2 * res["npv"] + dn) / (h * h),
            }
    except RuntimeError as exc:
        return Mark(
            None, spec.currency, inputs + [vinput], note=f"script pricing failed: {exc}"
        )
    state = scripted_payoffs.path_state(spec.product, params, done)
    past = [
        ModelParam(
            k,
            v,
            status="observed",
            source=f"{u} closes {start} → {min(d, spec.expiry)}",
        )
        for k, v in state.items()
    ]
    if stale:
        past.append(
            ModelParam(
                "fixings on a stale close",
                float(stale),
                status="stale",
                source="no close stored that day: the previous one used",
            )
        )
    contract = [
        ModelParam("start", text=start.isoformat(), status="contract"),
        ModelParam(
            "observations",
            float(len(t.dates)),
            status="contract",
            source=f"every {scripted_payoffs.CALENDARS.get(spec.currency, 'weekday')} business day, at the close",
        ),
    ]
    mc = [
        ModelParam(
            "paths",
            float(SCRIPT_PATHS),
            status="contract",
            source=f"seed {SCRIPT_SEED}, same draws every day",
        ),
    ]
    if smile is not None:
        model = "Dupire local volatility"
        why = (
            "Path-dependent payoffs depend on the forward smile, not one vol: local volatility "
            "reprices every vanilla of the day's surface and is the standard base for equity "
            "exotics. Past fixings are replayed, so a seasoned product is priced from where it is."
        )
        vparam = replace(
            vparam,
            name="local vol surface",
            value=None,
            text=f"{len(smile.K_grid)}×{len(smile.T_grid)} grid from the SVI fit",
        )
    else:
        model = "Black-Scholes, flat volatility"
        why = (
            "No option chain is stored for this underlying on this date, so no smile to calibrate: "
            "flat Black-Scholes at the realised-volatility proxy. Past fixings are replayed, so a "
            "seasoned product is priced from where it is."
        )
    # A fixing on a stale close makes the mark rest on stale data: the row's
    # input badge says so, not only the model's details.
    fix_inputs = (
        [Input(f"fixings {u} on a stale close", "stale", float(stale), d)]
        if stale
        else []
    )
    return Mark(
        float(res["npv"]),
        spec.currency,
        inputs + [vinput] + fix_inputs,
        greeks,
        model=ModelInfo(
            model,
            "Monte-Carlo on the product's script (Savine scripting engine)",
            why,
            _market_params(params, inputs) + [vparam] + contract + past + mc,
            std_error=float(res["mc_std_error"]),
        ),
    )


Pricer = Callable[..., Mark]
POLICY: Dict[str, Pricer] = {
    "vanilla": _vanilla,
    "american_vanilla": _vanilla,
    "digital": _digital,
    "quanto": _quanto,
    "future": _future,
}


def price(
    spec: DerivativeSpec,
    d: date,
    md: "MarketData",
    params: dict,
    inputs: List[Input],
    today: date,
) -> Optional[Mark]:
    """The desk model's mark, or None for a product this policy does not
    cover (the caller then prices it with the catalog pricer as before)."""
    if spec.product in SCRIPTED:
        return _scripted(spec, d, md, params, inputs, today)
    pricer = POLICY.get(spec.product)
    return pricer(spec, d, md, params, inputs) if pricer else None


def model_notes() -> str:
    return __doc__ or ""


__all__ = ["POLICY", "SCRIPTED", "SCRIPT_PATHS", "SCRIPT_SEED", "price"]
