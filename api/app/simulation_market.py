"""
The market side of the simulation page (blueprint/wp/29-simulation-diagnostics.md
§4): what the stored SVI surface says at the dates and strikes the model is
measured on, to draw the model against it.

- The implied vol at log-moneyness k = ln(K / F_T), interpolated linearly in T
  in total variance between the two bracketing slices: the interpolation of
  SVISurface (market/svi_surface.hpp) and of vol_surface.svi_implied_vol_grid.
- The risk-neutral density of S_T, by Breeden-Litzenberger, q(K) =
  e^{rT} d^2C/dK^2, in Gatheral's closed form on the total variance w(k)
  (The Volatility Surface, 2006, eq. 2.6 with Gatheral and Jacquier 2014, §2.2):

      q(K) = g(k) / (K sqrt(2 pi w)) exp(-d_-^2 / 2),
      d_- = -k / sqrt(w) - sqrt(w) / 2,
      g(k) = (1 - k w' / (2w))^2 - (w'^2 / 4)(1/w + 1/4) + w'' / 2.

  The SVI derivatives are analytic, so there is no finite difference on noisy
  quotes.
- Whether that density is one: g >= 0 (no butterfly arbitrage) and the
  digital P(S_T > K) = N(d_-) - phi(d_-) w' / (2 sqrt(w)) inside [0, 1] (no
  call-spread arbitrage). A fit that fails either does not price a law, and
  the page does not draw it (a degenerate slice, rho at -1, was seen to imply
  P(S_T > K) = 1.09 at its lowest quote).

Only where the market is quoted: T between the first and last surface slices
(no extrapolation in time), k inside both bracketing slices' quoted bands
(`k_lo`, `k_hi`). Elsewhere: None.
"""

from __future__ import annotations

import math
from typing import Dict, List, Optional, Sequence, Tuple

Slice = Dict[str, float]


def _svi(k: float, s: Slice) -> Tuple[float, float, float]:
    """w, dw/dk, d^2w/dk^2 of one raw SVI slice."""
    x = k - s["m"]
    r = math.sqrt(x * x + s["sigma"] ** 2)
    w = s["a"] + s["b"] * (s["rho"] * x + r)
    dw = s["b"] * (s["rho"] + x / r)
    d2w = s["b"] * s["sigma"] ** 2 / r**3
    return w, dw, d2w


def _bracket(slices: Sequence[Slice], T: float) -> Optional[Tuple[Slice, Slice, float]]:
    """The two slices around T and the weight of the upper one; None outside
    [first, last] slice."""
    if len(slices) < 2 or not slices[0]["ttm"] <= T <= slices[-1]["ttm"]:
        return None
    i = 0
    while i + 2 < len(slices) and slices[i + 1]["ttm"] < T:
        i += 1
    s0, s1 = slices[i], slices[i + 1]
    return s0, s1, (T - s0["ttm"]) / (s1["ttm"] - s0["ttm"])


def total_variance(
    slices: Sequence[Slice], k: float, T: float
) -> Optional[Tuple[float, float, float]]:
    """w(k, T) and its first two k-derivatives, or None where not quoted.
    `slices` sorted by maturity (market_snapshot.surface_slices)."""
    b = _bracket(slices, T)
    if b is None:
        return None
    s0, s1, lam = b
    lo = max(s0.get("k_lo", -math.inf), s1.get("k_lo", -math.inf))
    hi = min(s0.get("k_hi", math.inf), s1.get("k_hi", math.inf))
    if not lo <= k <= hi:
        return None
    a, c = _svi(k, s0), _svi(k, s1)
    return tuple((1.0 - lam) * x + lam * y for x, y in zip(a, c))  # type: ignore[return-value]


def implied_vols(
    slices: Sequence[Slice], T: float, moneyness: Sequence[float]
) -> List[Optional[float]]:
    """Market implied vol at K = m F_T for each m."""
    out: List[Optional[float]] = []
    for m in moneyness:
        w = total_variance(slices, math.log(m), T)
        out.append(math.sqrt(w[0] / T) if w is not None and w[0] > 0 else None)
    return out


def density(
    slices: Sequence[Slice], T: float, forward: float, strikes: Sequence[float]
) -> List[Optional[float]]:
    """Risk-neutral density of S_T at each strike (per unit of S), or None."""
    out: List[Optional[float]] = []
    for K in strikes:
        if K <= 0:
            out.append(None)
            continue
        k = math.log(K / forward)
        ww = total_variance(slices, k, T)
        if ww is None or ww[0] <= 0:
            out.append(None)
            continue
        w, dw, d2w = ww
        sw = math.sqrt(w)
        g = (
            (1.0 - k * dw / (2.0 * w)) ** 2
            - 0.25 * dw * dw * (1.0 / w + 0.25)
            + 0.5 * d2w
        )
        d_minus = -k / sw - 0.5 * sw
        out.append(
            g * math.exp(-0.5 * d_minus * d_minus) / (K * math.sqrt(2.0 * math.pi * w))
        )
    return out


def is_density(
    slices: Sequence[Slice], T: float, forward: float, strikes: Sequence[float]
) -> bool:
    """No butterfly and no call-spread arbitrage at these strikes (where
    quoted): the density above is the law of S_T there."""
    for K in strikes:
        if K <= 0:
            continue
        k = math.log(K / forward)
        ww = total_variance(slices, k, T)
        if ww is None or ww[0] <= 0:
            continue
        w, dw, d2w = ww
        sw = math.sqrt(w)
        g = (
            (1.0 - k * dw / (2.0 * w)) ** 2
            - 0.25 * dw * dw * (1.0 / w + 0.25)
            + 0.5 * d2w
        )
        d_minus = -k / sw - 0.5 * sw
        phi = math.exp(-0.5 * d_minus * d_minus) / math.sqrt(2.0 * math.pi)
        digital = 0.5 * math.erfc(-d_minus / math.sqrt(2.0)) - phi * dw / (2.0 * sw)
        if g < -1e-9 or not -1e-6 <= digital <= 1.0 + 1e-6:
            return False
    return True
