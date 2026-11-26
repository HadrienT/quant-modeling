"""A multilevel pricing ends within its route's timeout (issue #383).

SPY's one-touch at 0.2 % of its price escalated its levels to the GPU's Sobol
cap, fell back to a CPU run from level 0 and refined for tens of minutes
after the route had answered 408. The route now hands the engine the time it
has left (cancellation.engine_time_limit), the multilevel loops stop there,
and the user reads why. The native module prices for real, on a synthetic
skewed surface; on a server with a GPU the same run goes there."""

import threading
import time
from datetime import date

import pytest

import quantmodeling as qm
from app import cancellation, pricing_service, product_templates
from app.schemas import ScriptedProductRequest

VALUATION = date(2026, 11, 25)
K = [60.0 + 5.0 * i for i in range(17)]  # 60..140
T = [0.0, 0.25, 0.5, 1.0, 2.0]
# A put skew that steepens at the short end.
SIGMA = [
    0.20 + 0.25 * max(0.0, (100.0 - k) / 40.0) * (1.0 + 0.5 * (t < 0.3))
    for k in K
    for t in T
]


def _one_touch(**kw):
    script = product_templates.render("one-touch", {}, VALUATION)
    return qm.price_script(
        script,
        100.0,
        0.03,
        0.01,
        0.0,
        VALUATION.isoformat(),
        n_paths=20_000,
        seed=7,
        model="local_vol",
        K_grid=K,
        T_grid=T,
        sigma_loc_flat=SIGMA,
        steps_per_year=252,
        **kw,
    )


def test_a_multilevel_run_stops_at_its_time_limit_and_says_so():
    # 1e-4 of the price is out of reach in two seconds on any device.
    t0 = time.perf_counter()
    r = _one_touch(mlmc_rel_eps=1e-4, device="auto", time_limit=2.0)
    elapsed = time.perf_counter() - t0
    # The level in progress finishes: a margin, not the minutes of #383.
    assert elapsed < 15.0, r["diagnostics"]
    codes = {w["code"] for w in r["warnings"]}
    assert "discretisation_bias_target" in codes, r["diagnostics"]
    assert r["discretisation_bias"] is not None
    assert r["discretisation_bias_std_error"] > 0.0
    assert "time limit" in r["diagnostics"]


def test_without_a_limit_the_default_target_is_reached_or_said_unreached():
    # The default route's target, on a run that needs no limit to end: the
    # bias it reports comes with its own error.
    r = _one_touch(mlmc_rel_eps=0.002, device="auto", time_limit=60.0)
    assert r["discretisation_bias_std_error"] > 0.0
    reached = "bias test failed" not in r["diagnostics"]
    codes = {w["code"] for w in r["warnings"]}
    assert reached == ("discretisation_bias_target" not in codes), r["diagnostics"]


def test_time_limit_must_be_non_negative():
    with pytest.raises(Exception, match="time_limit"):
        _one_touch(mlmc_rel_eps=0.002, time_limit=-1.0)


def test_the_route_hands_the_engine_the_time_it_has_left():
    req = ScriptedProductRequest(
        product="one-touch", ticker="SPY", rate=0.03, model="local_vol"
    )
    # Outside a route: no limit.
    assert pricing_service._multilevel(req, "local_vol")["time_limit"] == 0.0
    got = cancellation.run_with(
        threading.Event(),
        lambda: pricing_service._multilevel(req, "local_vol")["time_limit"],
        deadline=time.perf_counter() + 7.0,
    )
    assert 6.0 < got <= 7.0
    # A route that already gave up: the engine stops at once.
    late = cancellation.run_with(
        threading.Event(),
        lambda: pricing_service._multilevel(req, "local_vol")["time_limit"],
        deadline=time.perf_counter() - 1.0,
    )
    assert 0.0 < late <= 1e-3
