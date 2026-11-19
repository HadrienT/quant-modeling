"""Pricing to a precision (#331 steps 2-3): the stopping rules on a fake
pricer whose error is sigma / sqrt(N), and the market's half-spread in vol."""

import math
from types import SimpleNamespace

import pytest

from app import precision as pr
from app.schemas import PricingResponse, ScriptRequest

SIGMA = 20.0  # per-path dispersion of the fake pricer


def fake_price(bias=0.0, bias_err=0.001, seconds_per_path=0.0, calls=None, vega=100.0):
    def price(req):
        if calls is not None:
            calls.append(req)
        if seconds_per_path:
            pr.time.sleep(seconds_per_path * req.n_paths)
        npv = 10.0 + vega * (pr.VOL_SCALE.get() - 1.0)
        return PricingResponse(
            npv=npv,
            greeks={},
            diagnostics="d",
            mc_std_error=SIGMA / math.sqrt(req.n_paths),
            discretisation_bias=bias if req.bias_estimate else None,
            discretisation_bias_std_error=bias_err if req.bias_estimate else None,
        )

    return price


def req(**precision):
    return ScriptRequest(
        script="2027-11-19\n    pays 1\n",
        spot=100.0,
        vol=0.2,
        rate=0.03,
        precision=precision,
    )


def test_an_absolute_target_is_reached_with_the_predicted_paths():
    calls = []
    r = pr.price_to_precision(req(mode="absolute", value=0.05), fake_price(calls=calls))
    p = r.precision
    assert p.stopped == "target_reached"
    # N = (sigma / (eps / sqrt 2))^2
    assert p.paths == p.paths_needed == math.ceil((SIGMA / (0.05 / math.sqrt(2))) ** 2)
    assert p.achieved_error <= 0.05
    assert calls[-1].precision is None and calls[-1].n_paths == p.paths
    assert "precision" in r.diagnostics


def test_a_relative_target_is_a_share_of_the_pilot_price():
    r = pr.price_to_precision(req(mode="relative", value=0.01), fake_price())
    assert r.precision.target == pytest.approx(0.1)


def test_a_resolved_bias_beyond_half_the_target_stops_at_the_pilot():
    r = pr.price_to_precision(
        req(mode="absolute", value=0.05), fake_price(bias=0.04, bias_err=0.005)
    )
    assert r.precision.stopped == "bias_floor"
    assert r.precision.paths == pr.PILOT_PATHS


def test_an_unresolved_bias_does_not_stop_the_run():
    r = pr.price_to_precision(
        req(mode="absolute", value=0.05), fake_price(bias=0.04, bias_err=0.03)
    )
    assert r.precision.stopped == "target_reached"


def test_a_target_beyond_the_budget_uses_what_fits_and_says_how_many_it_needed():
    r = pr.price_to_precision(
        req(mode="absolute", value=0.002, time_budget_s=0.5),
        fake_price(seconds_per_path=2e-6),
    )
    p = r.precision
    assert p.stopped == "out_of_budget"
    assert p.paths < p.paths_needed
    assert p.seconds < 1.5


def test_market_mode_scales_the_half_spread_by_the_vega():
    quotes = [
        SimpleNamespace(
            ttm=1.0,
            strike=k,
            is_call=c,
            bid=b,
            ask=b + 0.2,
            has_iv=True,
            implied_vol=0.2,
        )
        for k in (95.0, 100.0, 105.0)
        for c, b in ((True, 5.0), (False, 4.0))
    ]
    r = pr.price_to_precision(
        req(mode="market", value=0.25),
        fake_price(vega=100.0),
        lambda: (quotes, 100.0, 0.0, 0.0, pr.date(2026, 11, 19)),
    )
    p = r.precision
    ds, atm, expiry, n = pr.delta_sigma(quotes, 100.0, 0.0, 0.0, 1.0)
    assert (p.delta_sigma, p.atm_vol, p.expiry) == pytest.approx((ds, atm, expiry))
    assert p.vega_scale == pytest.approx(100.0)
    assert p.target == pytest.approx(0.25 * 100.0 * ds / atm)


def test_delta_sigma_takes_the_tighter_side_per_strike():
    # same strikes: calls 10x wider than puts (stale calls, as SPY's 2026-11-18)
    quotes = []
    for k in (95.0, 100.0, 105.0):
        quotes.append(
            SimpleNamespace(
                ttm=0.5, strike=k, bid=5.0, ask=7.0, has_iv=True, implied_vol=0.2
            )
        )
        quotes.append(
            SimpleNamespace(
                ttm=0.5, strike=k, bid=4.0, ask=4.2, has_iv=True, implied_vol=0.2
            )
        )
    ds, atm, expiry, n = pr.delta_sigma(quotes, 100.0, 0.0, 0.0, 0.6)
    assert n == 3 and expiry == 0.5 and atm == 0.2

    # the put's 0.1 half-spread over each strike's vega, median over strikes
    def vega(k):
        d1 = (math.log(100.0 / k) + 0.5 * 0.04 * 0.5) / (0.2 * math.sqrt(0.5))
        return 100.0 * pr._norm_pdf(d1) * math.sqrt(0.5)

    expected = sorted(0.1 / vega(k) for k in (95.0, 100.0, 105.0))[1]
    assert ds == pytest.approx(expected, rel=1e-12)


def test_market_mode_without_a_chain_is_refused():
    with pytest.raises(pr.PrecisionUnavailable):
        pr.price_to_precision(req(mode="market"), fake_price())


def test_a_bias_the_run_resolves_is_named_as_the_floor():
    # the pilot cannot resolve 0.04 (error 0.02 at its paths), the run can
    def price(req):
        r = fake_price(
            bias=0.04, bias_err=0.02 if req.n_paths <= 4 * pr.PILOT_PATHS else 0.002
        )(req)
        return r

    r = pr.price_to_precision(req(mode="absolute", value=0.05), price)
    assert r.precision.stopped == "bias_floor"
    assert r.precision.paths > pr.PILOT_PATHS


def test_more_paths_than_the_schema_allows_is_out_of_budget():
    r = pr.price_to_precision(req(mode="absolute", value=1e-4), fake_price())
    assert r.precision.stopped == "out_of_budget"
    assert r.precision.paths <= pr.MAX_PATHS < r.precision.paths_needed
