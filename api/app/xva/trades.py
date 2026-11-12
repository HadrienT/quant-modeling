"""The trades of a netting set as the library reads them, and the response pieces built from its result."""

from __future__ import annotations

import math
from datetime import timedelta
from typing import TYPE_CHECKING, List, Optional, Tuple

import quantmodeling as qm

from .. import xva_fx

if TYPE_CHECKING:  # pragma: no cover — xva_equity imports the portfolio modules
    from ..xva_equity import AssetMarket
from ..xva_schemas import (
    Adjustments,
    Estimate,
    PricingExposure,
    XvaRequest,
)
from .base import XvaInputError, XvaUnavailable, _FX_KINDS
from .demos import _Spec
from .market import _Rates


def _discount(rates: _Rates, t: float) -> float:
    """P(0, t) on the bootstrapped curve, log-linear between its pillars."""
    times, dfs = [0.0] + rates.times, [1.0] + rates.dfs
    for k in range(1, len(times)):
        if t <= times[k]:
            w = (t - times[k - 1]) / (times[k] - times[k - 1])
            return math.exp((1.0 - w) * math.log(dfs[k - 1]) + w * math.log(dfs[k]))
    return dfs[-1]


def swap_script(rates: _Rates, tenor: int, fixed_rate: float, notional: float) -> str:
    """A payer swap, annual on both legs, in the payoff language: on each
    anniversary it pays the floating coupon fixed a year earlier against the
    fixed one, and fixes the next from the curve of that date. The coupon
    that starts today is known today."""

    def day(k: int) -> str:
        # Exactly t = k under the script's ACT/365F.
        return (rates.as_of + timedelta(days=365 * k)).isoformat()

    first = 1.0 / _discount(rates, 1.0) - 1.0
    lines = []
    for k in range(1, tenor + 1):
        lines.append(day(k))
        floating = f"{first:.10f}" if k == 1 else "libor"
        lines.append(f"    pays {notional:.0f} * ({floating} - {fixed_rate:.10f})")
        if k < tenor:
            lines.append(f"    libor = 1 / df({day(k + 1)}) - 1")
    return "\n".join(lines) + "\n"


def _script_specs(req: XvaRequest, rates: _Rates) -> List[_Spec]:
    """The scripts of the request as trades. Each is parsed here, so that a
    script that does not parse is the request's error, with the parser's
    message, and its last date is known."""
    specs = []
    for k, trade in enumerate(req.scripts):
        try:
            events = qm.validate_script(
                trade.script, rates.as_of.isoformat(), "ACT/365F"
            )["events"]
        except RuntimeError as exc:
            raise XvaInputError(f"Script {k + 1} ({trade.label}): {exc}") from exc
        if not events or events[-1]["t"] <= 0.0:
            raise XvaInputError(
                f"Script {k + 1} ({trade.label}) has no event after the valuation "
                f"date, {rates.as_of.isoformat()}"
            )
        specs.append(
            _Spec(
                trade.label,
                "script",
                True,
                events[-1]["t"],
                0.0,
                quantity=trade.quantity,
                script=trade.script,
            )
        )
    return specs


def _cashflow(s: _Spec) -> dict:
    """An amount already fixed, as qm.xva_netting_set takes it."""
    out = {
        "kind": "cashflow",
        "time": s.tenor,
        "amount": s.notional,
        "quantity": s.quantity,
        "currency": "domestic" if s.currency == "USD" else "foreign",
        "payer": s.payer,
    }
    if s.swap is not None:
        out["notional"], out["swap_end"] = s.swap
    return out


def _rate_trade(s, par: float, currency: Optional[str] = None) -> dict:
    """The engine's dict of a swap or European swaption struck at `par`, with
    annual fixed and floating legs; `currency` only in a two-currency set."""
    trade = {"kind": s.kind}
    if currency is not None:
        trade["currency"] = currency
    trade.update(
        tenor=s.tenor,
        fixed_rate=par,
        notional=s.notional,
        payer=s.payer,
        quantity=s.quantity,
        fixed_frequency=1,
        float_frequency=1,
    )
    trade["start" if s.kind == "swap" else "expiry"] = s.start
    return trade


def _trades(specs: List[_Spec], rates: _Rates) -> List[dict]:
    """The trades at the market: each fixed rate is the par rate of its swap
    on today's curve (the forward par rate for a swaption)."""
    out = []
    for s in specs:
        if s.kind == "script" and s.script is not None:
            out.append(
                {
                    "kind": "script",
                    "script": s.script,
                    "valuation_date": rates.as_of.isoformat(),
                    "quantity": s.quantity,
                }
            )
            continue
        if s.kind == "cashflow":
            out.append(_cashflow(s))
            continue
        start, tenor = s.par_of or (s.start, s.tenor)
        par = (
            s.rate
            if s.rate is not None
            else qm.price_swap(
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
        )
        if s.kind == "script":
            out.append(
                {
                    "kind": "script",
                    "script": swap_script(rates, int(s.tenor), par, s.notional),
                    "valuation_date": rates.as_of.isoformat(),
                    "quantity": s.quantity,
                    "fixed_rate": par,
                }
            )
            continue
        out.append(_rate_trade(s, par))
    return out


def _profile(e: dict) -> dict:
    keys = ("times", "ee", "ene", "pfe", "epe", "eepe")
    return {k: e[k] for k in keys + ("epe_error", "eepe_error", "pfe_error")}


def _pricing_exposure(e: dict) -> PricingExposure:
    return PricingExposure(
        **_profile(e),
        discounted_ee=e["discounted_ee"],
        discounted_ene=e["discounted_ene"],
        discounted_ee_error=e["discounted_ee_error"],
        quantile_levels=e["quantile_levels"],
        value_quantiles=e["value_quantiles"],
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
        fca_error=r["fca_error"],
        fba_error=r["fba_error"],
        fva_error=r["fva_error"],
        colva_error=r["colva_error"],
        mva_error=r["mva_error"],
        kva_error=r["kva_error"],
        cva_rule_of_thumb=r["cva_rule_of_thumb"],
    )


def _two_currencies(specs: List[_Spec]) -> bool:
    return any(s.kind in _FX_KINDS or s.currency != "USD" for s in specs)


def _has_asset(specs: List[_Spec]) -> bool:
    return any(s.kind == "equity_option" for s in specs)


def _asset_trades(
    specs: List[_Spec], rates: _Rates, asset: "AssetMarket"
) -> Tuple[List[dict], dict, float]:
    """The trades of a netting set with equity options (lot X14), what
    qm.xva_netting_set takes as `foreign` for the share, and the volatility
    of the share in the model: the one that gives the option the model is
    calibrated on its market Black volatility, once the interest rate's own
    moves are counted in the forward's."""
    try:
        fit = qm.cross_currency_fx_volatility(
            (rates.mean_reversion, rates.sigma),
            # The dividend yield is known: a rate that does not move.
            (rates.mean_reversion, 1e-10),
            (0.0, asset.correlation.value, 0.0),
            asset.calibration_expiry,
            asset.market_volatility,
            [],
        )
    except RuntimeError as exc:
        raise XvaUnavailable(str(exc)) from exc
    out = []
    for s in specs:
        if s.kind == "equity_option":
            out.append(
                {
                    "kind": "asset_option",
                    "strike": s.rate,
                    "expiry": s.tenor,
                    "is_call": s.payer,
                    "notional": s.notional,
                    "quantity": s.quantity,
                }
            )
        elif s.kind == "cashflow" and s.currency == "USD":
            out.append(_cashflow(s))
        elif s.kind in ("swap", "swaption") and s.currency == "USD":
            par = (
                s.rate
                if s.rate is not None
                else qm.price_swap(
                    rates.times,
                    rates.dfs,
                    rates.times,
                    rates.dfs,
                    s.start,
                    s.tenor,
                    0.0,
                    1,
                    1,
                    1.0,
                    True,
                    0.0,
                )["par_rate"]
            )
            out.append(_rate_trade(s, par, currency="domestic"))
        else:
            raise XvaInputError(
                f"Next to equity options the engine takes dollar swaps and European "
                f"swaptions, not a {s.kind.replace('_', ' ')}: its second factor is "
                "the share."
            )
    argument = {
        "currency": asset.ticker,
        "domestic_currency": "USD",
        "spot": asset.spot,
        "dividend_yield": asset.dividend_yield,
        "volatility": fit["spot_volatility"],
        "rate_correlation": asset.correlation.value,
        "index": asset.index,
    }
    return out, argument, fit["spot_volatility"]


def _two_currency_trades(
    specs: List[_Spec], rates: _Rates, foreign: xva_fx.TwoCurrencyMarket
) -> List[dict]:
    """The trades of a two-currency netting set at the market: swaps at the
    par rate of their own curve, a forward at the forward exchange rate, a
    cross-currency swap with both legs at par and its notionals at the spot.
    Each is worth zero today."""
    euro = foreign.rates
    out = []
    for s in specs:
        if s.kind == "fx_forward":
            out.append(
                {
                    "kind": "fx_forward",
                    # Covered interest parity, unless the trade has its rate.
                    "strike": (
                        s.rate
                        if s.rate is not None
                        else foreign.spot
                        * foreign.discount(s.tenor)
                        / qm.rate_curve_discount_factors(
                            rates.times, rates.dfs, [s.tenor]
                        )[0]
                    ),
                    "maturity": s.tenor,
                    "notional": s.notional,
                    "quantity": s.quantity if s.payer else -s.quantity,
                }
            )
        elif s.kind == "cross_currency_swap":
            out.append(
                {
                    "kind": "cross_currency_swap",
                    "maturity": s.tenor,
                    "foreign_notional": s.notional,
                    "domestic_notional": (
                        s.domestic_notional
                        if s.domestic_notional is not None
                        else s.notional * foreign.spot
                    ),
                    "foreign_rate": (
                        s.foreign_rate
                        if s.foreign_rate is not None
                        else xva_fx.par_coupon(euro.times, euro.dfs, s.tenor)
                    ),
                    "domestic_rate": (
                        s.rate
                        if s.rate is not None
                        else xva_fx.par_coupon(rates.times, rates.dfs, s.tenor)
                    ),
                    "frequency": 1,
                    "receive_foreign": s.payer,
                    "quantity": s.quantity,
                }
            )
        elif s.kind == "cashflow":
            out.append(_cashflow(s))
        elif s.kind in ("swap", "swaption"):
            in_euros = s.currency != "USD"
            times, dfs = (
                (euro.times, euro.dfs) if in_euros else (rates.times, rates.dfs)
            )
            par = (
                s.rate
                if s.rate is not None
                else qm.price_swap(
                    times, dfs, times, dfs, s.start, s.tenor, 0.0, 1, 1, 1.0, True, 0.0
                )["par_rate"]
            )
            out.append(
                _rate_trade(s, par, currency="foreign" if in_euros else "domestic")
            )
        else:
            raise XvaInputError(
                f"A {s.kind} is valued by regression, which the two-currency "
                "simulation does not do: it takes swaps, European swaptions, FX "
                "forwards and cross-currency swaps."
            )
    return out
