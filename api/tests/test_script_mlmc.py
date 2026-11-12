"""Multilevel Monte Carlo through the scripting API (blueprint WP 26 §8,
§9.4): on by default when the script suits it, to a share of the price; the
usual engine otherwise with the reason in the diagnostics; the
discretisation bias returned either way. The native module prices for real;
the database is a synthetic local-vol surface."""

import pytest
from pydantic import ValidationError

from app import pricing_service

from test_script_model_choice import VANILLA, _req, store  # noqa: F401 (fixture)


def test_multilevel_is_the_default_under_local_vol_and_returns_its_bias(store):
    resp = pricing_service.price_script(_req(VANILLA, ticker="TEST", model="local_vol"))
    # the default sampler, Sobol: multilevel QMC (WP 26 P5), 0.2 % of the price
    assert "multilevel QMC" in resp.diagnostics
    eps = 0.002 * resp.npv
    assert resp.mc_std_error <= 1.02 * eps / 2**0.5
    assert resp.discretisation_bias is not None
    assert 0.0 <= resp.discretisation_bias <= eps / 2**0.5 * 1.0001


def test_an_absolute_target_overrides_the_relative_one(store):
    resp = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", mlmc_eps=0.05, milstein=True)
    )
    assert "target RMS error 0.05" in resp.diagnostics
    assert resp.mc_std_error <= 1.02 * 0.05 / 2**0.5


def test_off_runs_the_usual_engine_and_estimates_its_bias_by_halving(store):
    resp = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", mlmc="off")
    )
    assert "multilevel" not in resp.diagnostics
    assert "steps halved" in resp.diagnostics
    assert resp.discretisation_bias is not None and resp.discretisation_bias > 0.0
    # and the multilevel price agrees within both errors and the biases
    ml = pricing_service.price_script(_req(VANILLA, ticker="TEST", model="local_vol"))
    assert resp.npv == pytest.approx(ml.npv, abs=0.15)


def test_black_scholes_has_no_bias_and_on_says_why_it_cannot_run():
    resp = pricing_service.price_script(
        _req(VANILLA, spot=100.0, vol=0.2, model="black_scholes")
    )
    assert "multilevel" not in resp.diagnostics
    assert resp.discretisation_bias == 0.0
    forced = pricing_service.price_script(
        _req(VANILLA, spot=100.0, vol=0.2, model="black_scholes", mlmc="on")
    )
    assert (
        "no multilevel Monte Carlo: the model has no time-step bias"
        in forced.diagnostics
    )


def test_no_bias_without_the_estimate(store):
    resp = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", mlmc="off", bias_estimate=False)
    )
    assert resp.discretisation_bias is None


def test_bad_targets_are_refused():
    with pytest.raises(ValidationError):
        _req(VANILLA, spot=100.0, vol=0.2, mlmc_eps=-1.0)
    with pytest.raises(ValidationError):
        _req(VANILLA, spot=100.0, vol=0.2, mlmc_rel_eps=0.0)


def test_heston_keeps_the_usual_engine_by_default_with_its_bias(store):
    # WP 26 §9.5: its levels couple poorly, so 'auto' leaves it out; its
    # grid refines, so its bias is now estimated.
    resp = pricing_service.price_script(_req(VANILLA, ticker="TEST", model="heston"))
    assert "multilevel" not in resp.diagnostics
    assert resp.discretisation_bias is not None and resp.discretisation_bias > 0.0


def test_on_runs_multilevel_under_heston(store):
    resp = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="heston", mlmc="on", mlmc_eps=0.5)
    )
    assert "multilevel QMC" in resp.diagnostics
    assert resp.discretisation_bias is not None
