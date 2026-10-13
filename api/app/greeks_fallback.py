"""Greeks a pricer does not return, by bumping and revaluing.

Most engines of the C++ library give their own greeks — closed forms,
pathwise or likelihood-ratio estimators, a tree's own nodes. Some give none
(a digital, a forward, a bond, a rainbow) or only a part (the PDE gives delta
and gamma). The Pricing page shows the five for every product that has the
input each one is the sensitivity to; this module fills what is missing, and
says so: a greek computed here is a finite difference, named as such next to
the table.

The conventions are those of the engines' own greeks, per unit:

- **delta, gamma**: first and second derivative to the spot — to a parallel
  move of all the spots of a multi-asset product, by the same amount;
- **vega**: to the volatility (to a parallel move of all of them);
- **theta**: minus the derivative to the maturity, per year;
- **rho**: to the interest rate (the domestic one of an FX product).

A Monte-Carlo pricing is bumped with the same seed, hence the same draws:
the difference is that of the payoff, not of the noise. Its standard error is
not estimated.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from typing import Callable, Dict, List, Optional, Tuple

from pydantic import BaseModel

from .schemas import PricingResponse

#: Relative move of the spot, absolute moves of the volatility and of the rate,
#: and the day the maturity is shortened by.
SPOT_MOVE = 0.01
VOL_MOVE = 0.01
RATE_MOVE = 1e-4
DAY = 1.0 / 365.0

GREEKS = ("delta", "gamma", "vega", "theta", "rho")
#: The request field each greek is the sensitivity to, first one present.
_SPOT = ("spot", "spots")
_VOL = ("vol", "vols")
_RATE = ("rate", "rate_domestic")

Price = Callable[[BaseModel], float]


def _field(req: BaseModel, names: Tuple[str, ...]) -> Optional[str]:
    return next((n for n in names if getattr(req, n, None) is not None), None)


def _moved(req: BaseModel, name: str, amount: float) -> Optional[BaseModel]:
    """The request with one input moved by `amount` (every element of a
    list by the same amount); None when the request does not validate any
    more (a rate below its floor, a maturity of zero)."""
    value = getattr(req, name)
    new = [v + amount for v in value] if isinstance(value, list) else value + amount
    try:
        return type(req).model_validate({**req.model_dump(), name: new})
    except ValueError:
        return None


def complete(
    req: BaseModel, response: PricingResponse, price: Price
) -> Tuple[PricingResponse, List[str]]:
    """The response with the greeks the engine left out, and their names.
    `price(request)` is one more valuation of the product."""
    have = response.greeks
    missing = {g for g in GREEKS if getattr(have, g) is None}
    if not missing:
        return response, []
    spot, vol, rate = _field(req, _SPOT), _field(req, _VOL), _field(req, _RATE)
    level = None
    if spot is not None:
        s = getattr(req, spot)
        level = sum(s) / len(s) if isinstance(s, list) else s

    # (key, field, move): the bumped valuations what is missing needs.
    bumps: Dict[str, Tuple[str, float]] = {}
    if spot is not None and level and missing & {"delta", "gamma"}:
        h = SPOT_MOVE * abs(level)
        bumps["spot_up"], bumps["spot_down"] = (spot, h), (spot, -h)
    if vol is not None and "vega" in missing:
        bumps["vol_up"], bumps["vol_down"] = (vol, VOL_MOVE), (vol, -VOL_MOVE)
    if rate is not None and "rho" in missing:
        bumps["rate_up"], bumps["rate_down"] = (rate, RATE_MOVE), (rate, -RATE_MOVE)
    if getattr(req, "maturity", None) is not None and "theta" in missing:
        if req.maturity > 2 * DAY:
            bumps["sooner"] = ("maturity", -DAY)
    requests = {k: _moved(req, name, move) for k, (name, move) in bumps.items()}
    todo = {k: r for k, r in requests.items() if r is not None}
    if not todo:
        return response, []
    # The engines release the GIL: the bumps run side by side.
    with ThreadPoolExecutor(max_workers=len(todo)) as pool:
        futures = {k: pool.submit(price, r) for k, r in todo.items()}
        values: Dict[str, float] = {}
        for k, f in futures.items():
            try:
                values[k] = f.result()
            except Exception:  # noqa: BLE001 — a bump the engine refuses: no greek
                pass

    base = response.npv
    found: Dict[str, float] = {}
    if {"spot_up", "spot_down"} <= values.keys():
        h = bumps["spot_up"][1]
        if "delta" in missing:
            found["delta"] = (values["spot_up"] - values["spot_down"]) / (2 * h)
        if "gamma" in missing:
            found["gamma"] = (values["spot_up"] - 2 * base + values["spot_down"]) / (
                h * h
            )
    if {"vol_up", "vol_down"} <= values.keys():
        found["vega"] = (values["vol_up"] - values["vol_down"]) / (2 * VOL_MOVE)
    if {"rate_up", "rate_down"} <= values.keys():
        found["rho"] = (values["rate_up"] - values["rate_down"]) / (2 * RATE_MOVE)
    if "sooner" in values:
        found["theta"] = (values["sooner"] - base) / DAY
    if not found:
        return response, []
    greeks = have.model_copy(update=found)
    return response.model_copy(update={"greeks": greeks}), sorted(
        found, key=GREEKS.index
    )


__all__ = ["complete", "GREEKS"]
