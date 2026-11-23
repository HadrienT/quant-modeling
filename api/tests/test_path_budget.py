"""Pricing what fits in the time limit (issue #347): the cost learnt from
the pricings already done, the path count lowered with a warning, and the
precision mode's not-converged warning."""

import asyncio
import math

import pytest

from app import path_budget as pb
from app import precision as pr
from app import valuation
from app.routers import pricing
from app.schemas import PricingResponse, ScriptRequest


@pytest.fixture(autouse=True)
def fresh():
    pb.forget()
    yield
    pb.forget()


def req(n=1_000_000, seed=42, spot=100.0, **over):
    return ScriptRequest(
        script="2027-11-19\n    pays 1\n",
        spot=spot,
        vol=0.2,
        rate=0.03,
        n_paths=n,
        seed=seed,
        **over,
    )


def run(engine_s, diagnostics="d"):
    resp = PricingResponse(
        npv=1.0, greeks={}, diagnostics=diagnostics, mc_std_error=0.1
    )
    return valuation.Priced(resp, [], engine_s, {"engine": engine_s})


def test_without_a_finished_run_the_paths_asked_are_priced():
    assert pb.paths_that_fit("script", req(5_000_000), 20.0) is None


def test_a_slow_finished_run_bounds_the_cost_of_the_next():
    pb.record("script", req(1_000_000), run(4.0))  # 4e-6 s a path, at most
    fit, predicted = pb.paths_that_fit("script", req(5_000_000, seed=7), 20.0)
    assert predicted == pytest.approx(20.0)
    assert fit == pytest.approx(pb.ENGINE_SHARE * 20.0 / 4e-6, abs=1)
    # what fits is priced as asked
    assert pb.paths_that_fit("script", req(2_000_000), 20.0) is None


def test_a_short_run_teaches_nothing():
    # 0.5 s of mostly fixed costs: t / n would over-state a path's cost
    pb.record("script", req(50_000), run(0.5))
    assert pb.paths_that_fit("script", req(5_000_000), 20.0) is None


def test_another_request_does_not_share_the_cost():
    pb.record("script", req(1_000_000), run(4.0))
    assert pb.paths_that_fit("script", req(5_000_000, spot=90.0), 20.0) is None
    assert pb.paths_that_fit("vanilla", req(5_000_000), 20.0) is None


def test_the_tightest_bound_is_kept():
    pb.record("script", req(1_000_000), run(6.0))
    pb.record("script", req(2_000_000), run(8.0))  # 4e-6: fixed costs weigh less
    fit, _ = pb.paths_that_fit("script", req(5_000_000), 20.0)
    assert fit == pytest.approx(pb.ENGINE_SHARE * 20.0 / 4e-6, abs=1)


def test_a_multilevel_run_teaches_nothing():
    pb.record("script", req(1_000_000), run(4.0, "multilevel QMC (levels 2..5)"))
    assert pb.paths_that_fit("script", req(5_000_000), 20.0) is None


def test_a_precision_request_is_left_to_its_own_budget():
    r = req(precision={"mode": "absolute", "value": 0.01})
    pb.record("script", r, run(10.0))
    assert pb.paths_that_fit("script", r, 20.0) is None


def test_the_warning_names_the_paths_and_the_error_scale():
    r = PricingResponse(npv=1.0, greeks={}, diagnostics="d", mc_std_error=0.1)
    w = pb.warn(r, 5_000_000, 1_250_000, 40.0, 20.0)
    (msg,) = [x.message for x in w.warnings if x.code == "time_limit_paths"]
    assert "1,250,000 of the 5,000,000" in msg and "2.0x" in msg
    assert "1250000 of the 5000000" in w.diagnostics


def test_the_route_prices_what_fits_and_records_it(monkeypatch):
    seen = []

    def fake_price(product_id, r):
        seen.append(r.n_paths)
        resp = PricingResponse(npv=1.0, greeks={}, diagnostics="d", mc_std_error=0.1)
        return valuation.Priced(resp, [], 0.1, {"engine": 4e-6 * r.n_paths})

    monkeypatch.setattr(valuation, "price", fake_price)
    monkeypatch.setattr(pricing, "emit", lambda *a, **k: None)
    first = asyncio.run(pricing._price("script", req(1_000_000)))
    assert seen == [1_000_000] and not first.warnings
    second = asyncio.run(pricing._price("script", req(5_000_000)))
    assert seen[-1] == pytest.approx(
        pb.ENGINE_SHARE * pricing._PRICING_TIMEOUT_SECONDS / 4e-6, abs=1
    )
    assert [w.code for w in second.warnings] == ["time_limit_paths"]


def _precision_price(req_):
    return PricingResponse(
        npv=10.0,
        greeks={},
        diagnostics="d",
        mc_std_error=20.0 / math.sqrt(req_.n_paths),
    )


def test_precision_stopped_by_the_time_limit_warns_it_did_not_converge():
    r = pr.price_to_precision(
        req(precision={"mode": "absolute", "value": 1e-4}), _precision_price
    )
    assert r.precision.stopped == "out_of_budget"
    (w,) = [x for x in r.warnings if x.code == "time_limit_precision"]
    assert w.severity == "warning" and "Not converged" in w.message
    assert f"{r.precision.paths:,} paths of the ~" in w.message


def test_a_precision_target_reached_has_no_warning():
    r = pr.price_to_precision(
        req(precision={"mode": "absolute", "value": 0.05}), _precision_price
    )
    assert r.precision.stopped == "target_reached"
    assert not [x for x in r.warnings if x.code.startswith("time_limit")]


def test_the_sobol_rounding_matches_what_the_engine_ran():
    # utils/sobol.hpp, as the diagnostics showed: 300,000 -> 16 x 16,384 and
    # 5,000,000 -> 16 x 262,144 (nearest power of two, geometrically)
    assert pb._rounded(300_000, 16) == 16 * 16_384
    assert pb._rounded(5_000_000, 16) == 16 * 262_144
    assert pb._rounded(700_000, 16) == 16 * 32_768
    assert pb._rounded(12_345, None) == 12_345


def test_a_sobol_run_is_costed_on_its_paths_and_lowered_to_an_exact_count():
    sobol = "SimulationMCEngine + Sobol RQMC (16 batches of 16384 points, dim=26)"
    pb.record("script", req(300_000), run(6.7, sobol))  # 262,144 paths ran
    fit, predicted = pb.paths_that_fit("script", req(2_000_000), 20.0)
    per_path = 6.7 / (16 * 16_384)
    assert predicted == pytest.approx(per_path * 16 * 131_072)
    # 16 x a power of two, so the engine runs exactly the paths announced
    assert fit % 16 == 0 and pb._rounded(fit, 16) == fit
    assert fit * per_path <= pb.ENGINE_SHARE * 20.0
