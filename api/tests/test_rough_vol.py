"""Rough Bergomi on the stored surface (rough_vol.py): the forward variance
curve is the variance swap's, replicated from the SVI smile, and the model
prices scripts once calibrated. The real native module calibrates; only the
database is replaced, by a synthetic market surface."""

import math
from datetime import date

import pytest

from app import market_snapshot, pricing_service, rough_vol
from app.schemas import ScriptRequest

SNAP = date(2026, 9, 11)


def _svi(T: float, skew: bool = True) -> dict:
    """A 20 % ATM vol; with `skew`, an equity put skew."""
    if not skew:
        return dict(ttm=T, a=0.04 * T, b=0.0, rho=0.0, m=0.0, sigma=0.1)
    return dict(ttm=T, a=0.03 * T, b=0.1 * T, rho=-0.6, m=0.0, sigma=0.1)


def test_a_flat_smile_has_its_own_variance_as_variance_swap():
    sl = _svi(0.25, skew=False)
    assert rough_vol.variance_swap_total_variance(sl) == pytest.approx(0.01, rel=1e-6)


def test_a_put_skew_puts_the_variance_swap_above_the_money():
    """The reason xi0 is not the ATM curve: under a negative skew the
    log-contract variance exceeds the ATM implied variance."""
    sl = _svi(0.5)
    w_atm = rough_vol._w(sl, 0.0)
    assert rough_vol.variance_swap_total_variance(sl) > 1.02 * w_atm


def _market() -> market_snapshot.LocalVolMarket:
    ttms = (0.03, 0.08, 0.16, 0.25, 0.5, 1.0)
    return market_snapshot.LocalVolMarket(
        ticker="TEST",
        valuation_date=SNAP,
        spot=100.0,
        dividend=0.0,
        K_grid=[50.0 + 5.0 * i for i in range(21)],
        T_grid=[0.1, 0.5, 1.0],
        sigma_loc_flat=[0.2] * 63,
        svi_slices=[_svi(T) for T in ttms],
        rate=0.03,
    )


def test_targets_cover_the_short_end_only():
    tgt, ttm, w = rough_vol.targets(_market())
    assert {round(t, 2) for t, _, _ in tgt} == {0.03, 0.08, 0.16, 0.25, 0.5}
    assert ttm == sorted(ttm) and max(ttm) <= rough_vol.MAX_TTM
    assert all(b > a for a, b in zip(w, w[1:]))  # total variance increases


def test_a_script_prices_under_the_calibrated_rough_bergomi(monkeypatch):
    m = _market()
    monkeypatch.setattr(market_snapshot, "local_vol_market", lambda *a: m)
    monkeypatch.setattr(rough_vol, "_load", lambda key: None)
    monkeypatch.setattr(rough_vol, "_save", lambda key, rv: None)
    req = ScriptRequest(
        script="2027-03-11\n    pays max(spot() - 100, 0)\n",
        model="rough_bergomi",
        ticker="TEST",
        rate=0.03,
        valuation_date=SNAP,
        n_paths=20_000,
    )
    r = pricing_service.price_script(req)
    cal = r.model_choice.calibration
    assert r.model_choice.model == "rough_bergomi"
    assert cal.rough_bergomi is not None and 0.0 < cal.rough_bergomi.H < 0.5
    assert cal.rough_bergomi.iv_rmse < 0.03  # within three vol points of SVI
    # A 6-month ATM call on a ~20 % vol: Black-Scholes would give ~6.3.
    assert 4.0 < r.npv < 9.0
    assert math.isfinite(r.mc_std_error)
