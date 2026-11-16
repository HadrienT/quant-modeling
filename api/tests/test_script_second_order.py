"""Second-order risks of a script through the API (blueprint/wp/27-ir-ad.md
§14.10): second_order=True with greeks_method='aad' returns gamma, vanna and
volga with their standard errors and the method that gave them -- the bump
on the GPU's adjoint. On a server without a GPU (the CI) the first-order
answer stands and the diagnostics say why. The native module prices for
real."""

import math
from datetime import date

import pytest

import quantmodeling as qm
from app import pricing_service

from test_script_model_choice import SNAP, VANILLA, _req

HAS_GPU = bool(qm.gpu_devices())


def _black_scholes(S, K, r, vol, T):
    d1 = (math.log(S / K) + (r + 0.5 * vol * vol) * T) / (vol * math.sqrt(T))
    d2 = d1 - vol * math.sqrt(T)
    n1 = math.exp(-0.5 * d1 * d1) / math.sqrt(2.0 * math.pi)
    gamma = n1 / (S * vol * math.sqrt(T))
    vanna = -n1 * d2 / vol
    volga = S * n1 * math.sqrt(T) * d1 * d2 / vol
    return gamma, vanna, volga


def _priced(**kw):
    return pricing_service.price_script(
        _req(
            VANILLA,
            spot=100.0,
            vol=0.2,
            model="black_scholes",
            greeks_method="aad",
            sampler="sobol",
            n_paths=1 << 18,
            **kw,
        )
    )


def test_off_by_default():
    resp = _priced()
    assert resp.second_order_method is None
    assert resp.greeks.vanna is None and resp.greeks.volga is None


@pytest.mark.skipif(not HAS_GPU, reason="the second order runs on the GPU")
def test_gamma_vanna_volga_are_black_scholess():
    resp = _priced(second_order=True, device="auto")
    assert resp.second_order_method is not None
    assert "bump on the adjoint" in resp.second_order_method
    T = (date(2027, 9, 13) - SNAP).days / 365.0
    gamma, vanna, volga = _black_scholes(100.0, 100.0, 0.03, 0.2, T)
    g = resp.greeks
    assert g.gamma == pytest.approx(gamma, abs=4 * g.gamma_std_error + 0.01 * gamma)
    assert g.vanna == pytest.approx(
        vanna, abs=4 * g.vanna_std_error + 0.02 * abs(vanna)
    )
    assert g.volga == pytest.approx(
        volga, abs=4 * g.volga_std_error + 0.02 * abs(volga)
    )
    assert 0.0 < g.gamma_std_error < 0.05 * gamma


def test_on_the_cpu_the_first_order_answer_stands_and_says_why():
    resp = _priced(second_order=True, device="cpu")
    assert resp.second_order_method is None
    assert resp.greeks.vanna is None
    assert "no second-order risks" in resp.diagnostics
    assert resp.risks  # the first-order report is still there


@pytest.mark.skipif(not HAS_GPU, reason="the second order runs on the GPU")
def test_a_request_that_names_no_device_gets_its_second_order():
    # The scripting page sends no device when the user leaves it on Auto:
    # the second order must not be dropped for the CPU then.
    resp = _priced(second_order=True)
    assert resp.second_order_method is not None
    assert resp.greeks.gamma is not None and resp.greeks.vanna is not None


def _digital_gamma(S, K, r, vol, T):
    d2 = (math.log(S / K) + (r - 0.5 * vol * vol) * T) / (vol * math.sqrt(T))
    d1 = d2 + vol * math.sqrt(T)
    n2 = math.exp(-0.5 * d2 * d2) / math.sqrt(2.0 * math.pi)
    return -math.exp(-r * T) * n2 * d1 / (S * S * vol * vol * T)


def test_a_hard_digital_gets_its_gamma_by_vibrato_on_the_cpu():
    resp = pricing_service.price_script(
        _req(
            "2027-09-13\n    if spot() > 100 then pays 1 endIf\n",
            spot=100.0,
            vol=0.2,
            model="black_scholes",
            greeks_method="aad",
            second_order=True,
            device="cpu",
            n_paths=100_000,
        )
    )
    assert resp.second_order_method is not None
    assert "Vibrato" in resp.second_order_method
    T = (date(2027, 9, 13) - SNAP).days / 365.0
    gamma = _digital_gamma(100.0, 100.0, 0.03, 0.2, T)
    g = resp.greeks
    assert g.gamma == pytest.approx(
        gamma, abs=4 * g.gamma_std_error + 0.01 * abs(gamma)
    )


def test_a_jump_before_the_final_date_gets_no_second_order_and_says_why():
    resp = pricing_service.price_script(
        _req(
            "2027-03-11\n    if spot() < 80 then out = 1 endIf\n"
            "2027-09-13\n    if out = 0 then pays max(spot() - 100, 0) endIf\n",
            spot=100.0,
            vol=0.2,
            model="black_scholes",
            greeks_method="aad",
            second_order=True,
            device="auto",
        )
    )
    assert resp.second_order_method is None
    assert resp.greeks.vanna is None
    assert "before its final date" in resp.diagnostics
