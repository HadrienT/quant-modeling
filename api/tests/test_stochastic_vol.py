"""The Heston fit's targets (stochastic_vol.heston_targets): read off the SVI
slices only where each slice was quoted (#97) -- beyond, SVI extrapolates and
a target there is the fit's own invention."""

import math
from types import SimpleNamespace

from app import stochastic_vol as sv


def _slice(T, k_lo, k_hi):
    return {
        "ttm": T,
        "a": 0.02 * T,
        "b": 0.1,
        "rho": -0.6,
        "m": 0.0,
        "sigma": 0.2,
        "quotes": [(k_lo, 0.5, 1.0), (0.0, 0.5, 1.0), (k_hi, 0.5, 1.0)],
    }


def test_targets_stay_inside_each_slices_quoted_range():
    market = SimpleNamespace(
        spot=100.0,
        rate=0.03,
        dividend=0.01,
        svi_slices=[_slice(0.5, -0.30, 0.10), _slice(2.0, -0.40, 0.05)],
    )
    strikes, ttms, vols = sv.heston_targets(market)
    assert len(strikes) == len(ttms) == len(vols) > 0
    bounds = {0.5: (-0.30, 0.10), 2.0: (-0.40, 0.05)}
    for K, T in zip(strikes, ttms):
        forward = 100.0 * math.exp((0.03 - 0.01) * T)
        k = math.log(K / forward)
        lo, hi = bounds[T]
        assert lo - 1e-12 <= k <= hi + 1e-12
    # the call wing beyond +0.05 at 2 years is dropped, the puts kept
    assert sum(1 for T in ttms if T == 2.0) < len(sv.Z_POINTS)


def test_short_maturities_are_left_out():
    market = SimpleNamespace(
        spot=100.0,
        rate=0.0,
        dividend=0.0,
        svi_slices=[_slice(0.01, -0.5, 0.5), _slice(1.0, -0.5, 0.5)],
    )
    _, ttms, _ = sv.heston_targets(market)
    assert set(ttms) == {1.0}
