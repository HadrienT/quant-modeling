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


def test_a_multilevel_price_takes_the_gpu_unless_told_otherwise(store):
    """WP 26 §10: a request that names no device runs its multilevel price
    on the GPU when the server has one; every other run, and AAD greeks,
    stay on the CPU; a named device is obeyed."""
    import quantmodeling as qm

    has_gpu = bool(qm.gpu_devices())
    default = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol")
    )
    assert "multilevel QMC" in default.diagnostics
    assert default.device == ("gpu" if has_gpu else "cpu")

    cpu = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", device="cpu")
    )
    assert cpu.device == "cpu" and "multilevel QMC" in cpu.diagnostics
    # the same estimator on the other device: within both errors
    assert default.npv == pytest.approx(
        cpu.npv, abs=4.0 * (default.mc_std_error + cpu.mc_std_error)
    )

    off = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", mlmc="off")
    )
    assert off.device == "cpu"


def test_aad_greeks_come_from_the_multilevel_levels_on_the_gpu(store):
    """WP 26 §10.2: under local vol the risks of a multilevel price are the
    sum of its levels' adjoints, on the GPU; without one the plain engine
    computes them and the diagnostics say so."""
    import quantmodeling as qm

    resp = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", greeks_method="aad")
    )
    assert resp.risks is not None and resp.greeks.delta is not None
    if not qm.gpu_devices():
        assert resp.device == "cpu"
        assert "used the plain engine" in resp.diagnostics
        return
    assert resp.device == "gpu"
    assert "multilevel QMC" in resp.diagnostics
    assert "risks by the per-path adjoint of every level" in resp.diagnostics
    assert resp.discretisation_bias is not None
    # the same levels as the price alone: the same number
    price = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol")
    )
    assert resp.npv == pytest.approx(price.npv, abs=1e-9)
    # and the plain engine's delta within both errors
    plain = pricing_service.price_script(
        _req(VANILLA, ticker="TEST", model="local_vol", greeks_method="aad", mlmc="off")
    )
    assert "multilevel" not in plain.diagnostics
    tol = 4.0 * (resp.greeks.delta_std_error + plain.greeks.delta_std_error) + 2e-3
    assert resp.greeks.delta == pytest.approx(plain.greeks.delta, abs=tol)
