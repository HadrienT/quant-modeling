"""The Heston fit's targets (stochastic_vol.heston_targets): read off the SVI
slices only where each slice was quoted (#97) -- beyond, SVI extrapolates and
a target there is the fit's own invention."""

import dataclasses
import math
from datetime import date
from types import SimpleNamespace

import pytest

from app import market_snapshot
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


def test_only_the_dupire_grids_slices_are_targets():
    """#119: Heston (and SLV on top of it) sees the local vol's surface, not
    the slices select_surface_slices left out for calendar arbitrage."""
    dropped = dict(_slice(1.0, -0.5, 0.5), in_surface=False, a=0.2)
    market = SimpleNamespace(
        spot=100.0,
        rate=0.0,
        dividend=0.0,
        svi_slices=[
            dict(_slice(0.5, -0.5, 0.5), in_surface=True),
            dropped,
            dict(_slice(2.0, -0.5, 0.5), in_surface=True),
        ],
    )
    _, ttms, _ = sv.heston_targets(market)
    assert set(ttms) == {0.5, 2.0}


# -- A fit that does not reprice its surface is refused (issue #146) ----------


def _market() -> market_snapshot.LocalVolMarket:
    K = [50.0 + 5.0 * i for i in range(21)]
    T = [0.1, 0.5, 1.0, 2.0]
    return market_snapshot.LocalVolMarket(
        ticker="TEST",
        valuation_date=date(2026, 10, 2),
        spot=100.0,
        dividend=0.0,
        K_grid=K,
        T_grid=T,
        sigma_loc_flat=[0.2 for _ in K for _ in T],
        svi_slices=[
            dict(ttm=t, a=0.03 * t, b=0.1 * t, rho=-0.6, m=0.0, sigma=0.1)
            for t in (0.1, 0.25, 0.5, 1.0, 2.0)
        ],
        rate=0.03,
    )


def _fit(iv_rmse: float) -> dict:
    """What the 2026-10-02 SPY snapshot gave: parameters on their bounds."""
    return dict(
        v0=1e-4,
        kappa=0.244,
        theta=1.81,
        xi=4.0,
        rho=0.99,
        iv_rmse=iv_rmse,
        iv_worst=0.331,
        n_quotes=19,
        n_maturities=3,
        feller=False,
    )


def test_a_fit_that_does_not_reprice_the_surface_is_refused(monkeypatch):
    def no_leverage(*args, **kwargs):
        raise AssertionError("no leverage is built on a refused Heston")

    monkeypatch.setattr(sv.qm, "calibrate_heston", lambda *a, **k: _fit(0.173))
    monkeypatch.setattr(sv.qm, "calibrate_slv_leverage", no_leverage)
    sv._cache.clear()
    with pytest.raises(sv.CalibrationUnavailable) as refused:
        sv.calibrate(_market())
    message = str(refused.value)
    assert "17.3 vol points" in message
    assert "19 points on 3 maturities" in message
    # nothing was stored: the refusal is not a calibration
    assert sv._load(sv.store_key(_market())) is None


def test_the_limit_itself_is_accepted(monkeypatch):
    monkeypatch.setattr(sv.qm, "calibrate_heston", lambda *a, **k: _fit(sv.MAX_IV_RMSE))
    monkeypatch.setattr(sv, "SLV_PARTICLES", 2_000)
    sv._cache.clear()
    assert sv.calibrate(_market()).iv_rmse == sv.MAX_IV_RMSE


def test_a_poor_fit_stored_before_the_rule_is_refused_too(monkeypatch):
    monkeypatch.setattr(sv.qm, "calibrate_heston", lambda *a, **k: _fit(sv.MAX_IV_RMSE))
    monkeypatch.setattr(sv, "SLV_PARTICLES", 2_000)
    sv._cache.clear()
    accepted = sv.calibrate(_market())
    key = sv.store_key(_market())
    sv._save(key, dataclasses.replace(accepted, iv_rmse=0.15))
    sv._cache.clear()  # a new process: only the store is left

    def no_fit(*args, **kwargs):
        raise AssertionError("the stored fit is read, not redone")

    monkeypatch.setattr(sv.qm, "calibrate_heston", no_fit)
    with pytest.raises(sv.CalibrationUnavailable, match="15.0 vol points"):
        sv.calibrate(_market())
