"""Multilevel Monte Carlo through the scripting API (blueprint WP 26 §8,
P4c): opt-in with mlmc_eps, applied when the script suits it, the usual
engine otherwise with the reason in the diagnostics. The native module
prices for real; the database is a synthetic local-vol surface."""

import pytest
from pydantic import ValidationError

from app import pricing_service

from test_script_model_choice import VANILLA, _req, store  # noqa: F401 (fixture)


def test_mlmc_runs_under_local_vol_and_says_so(store):
    resp = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", mlmc_eps=0.05, milstein=True)
    )
    # the default sampler, Sobol: multilevel QMC (WP 26 P5)
    assert "multilevel QMC" in resp.diagnostics
    assert "target RMS error 0.05" in resp.diagnostics
    assert resp.mc_std_error <= 1.02 * 0.05 / 2**0.5
    # the same price as the default route, within both errors and the bias
    plain = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", n_paths=200_000)
    )
    assert resp.npv == pytest.approx(plain.npv, abs=0.15)


def test_mlmc_off_by_default_leaves_the_route_unchanged(store):
    a = pricing_service.price_script(_req(VANILLA, ticker="TEST", model="local_vol"))
    b = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", mlmc_eps=0.0)
    )
    assert a.npv == b.npv
    assert "multilevel" not in a.diagnostics


def test_mlmc_under_black_scholes_falls_back_and_says_why():
    resp = pricing_service.price_script(
        _req(VANILLA, spot=100.0, vol=0.2, model="black_scholes", mlmc_eps=0.05)
    )
    assert (
        "no multilevel Monte Carlo: the model has no time-step bias" in resp.diagnostics
    )


def test_a_negative_target_is_refused():
    with pytest.raises(ValidationError):
        _req(VANILLA, spot=100.0, vol=0.2, mlmc_eps=-1.0)
