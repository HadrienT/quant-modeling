"""model='hull_white' on the scripting endpoint: the equity under stochastic
Hull-White rates fitted to the currency's government curve from the
database. The real native module prices; only the curve lookup is replaced."""

import math
from datetime import date

import pytest
from pydantic import ValidationError

from app import pricing_service, rates
from app.schemas import ScriptRequest

TODAY = date(2026, 9, 29)
ZERO = 0.035  # a flat continuously compounded curve, for the checks below


@pytest.fixture
def curve(monkeypatch):
    times = [0.25, 0.5, 1, 2, 3, 5, 7, 10, 20, 30]
    snap = rates.DiscountCurveSnapshot(
        "USD", TODAY, times, [math.exp(-ZERO * t) for t in times]
    )
    monkeypatch.setattr(rates, "currency_discount_curve", lambda c: snap)
    return snap


def _req(script: str, **kw) -> ScriptRequest:
    base = dict(
        script=script,
        model="hull_white",
        spot=100.0,
        vol=0.2,
        rate=0.0,  # ignored: the curve is the database's
        valuation_date=TODAY,
        n_paths=100_000,
        seed=5,
    )
    return ScriptRequest(**{**base, **kw})


def test_a_zero_coupon_bond_is_the_curve_whatever_the_rate_vol(curve):
    for sigma in (0.005, 0.02):
        r = pricing_service.price_script(
            _req("2031-09-29\n    pays 1\n", hull_white={"sigma": sigma})
        )
        T = (date(2031, 9, 29) - TODAY).days / 365.0
        assert r.npv == pytest.approx(
            math.exp(-ZERO * T), abs=4 * r.mc_std_error + 1e-9
        )
    assert r.model_choice.model == "hull_white"
    assert "hull_white" in r.diagnostics


def test_a_correlated_call_gains_from_positive_rho(curve):
    call = "2036-09-29\n    pays max(spot() - 150, 0)\n"
    lo = pricing_service.price_script(
        _req(call, hull_white={"rho": -0.6, "sigma": 0.015})
    )
    hi = pricing_service.price_script(
        _req(call, hull_white={"rho": 0.6, "sigma": 0.015})
    )
    assert hi.npv - lo.npv > 4 * math.hypot(hi.mc_std_error, lo.mc_std_error)


def test_a_currency_without_a_curve_is_an_error_not_a_flat_rate(monkeypatch):
    def unavailable(c):
        raise rates.RatesUnavailable("CHF: no free curve")

    monkeypatch.setattr(rates, "currency_discount_curve", unavailable)
    with pytest.raises(ValueError, match="needs the CHF discount curve"):
        pricing_service.price_script(
            _req("2027-09-29\n    pays 1\n", hull_white={"currency": "CHF"})
        )


def test_hull_white_without_spot_and_vol_needs_a_ticker():
    with pytest.raises(ValidationError, match="requires spot and vol"):
        ScriptRequest(script="2027-09-29\n    pays 1\n", model="hull_white", rate=0.0)
