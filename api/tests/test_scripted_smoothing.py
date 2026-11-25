"""The measured smoothing of the library's adjoint greeks (`# eps:` /
`# greeks:` headers, pricing_service._price_library_script, WP 26 §5).

Properties: a product whose conditions jump gets greeks that see the jump
(a one-touch's delta is not zero); "separate" takes the price from an
unsmoothed run -- the hard price exactly -- and the greeks from a smoothed
one; "joint" prices and differentiates one smoothed run; a request that
sets fuzzy itself is priced as it asks; the widths are where the bench put
them.
"""

from datetime import date

import pytest

from app import pricing_service as ps
from app import product_templates as pt
from app.schemas import ScriptedProductRequest

V = date(2026, 6, 1)


def _req(product, **kw):
    base = dict(
        product=product,
        valuation_date=V,
        model="black_scholes",
        spot=100.0,
        vol=0.25,
        rate=0.03,
        dividend=0.0,
        n_paths=20_000,
        seed=3,
        greeks_method="aad",
    )
    return ScriptedProductRequest(**{**base, **kw})


def test_the_widths_are_read_from_the_headers():
    assert pt.get("one-touch").eps == 1.0 and pt.get("one-touch").greeks == "separate"
    assert pt.get("ladder-call").eps == 0.3 and pt.get("ladder-call").greeks == "joint"
    assert pt.get("european-call").eps is None


def test_separate_prices_unsmoothed_and_differentiates_smoothed():
    r = ps.price_scripted_product(_req("one-touch"))
    hard = ps.price_scripted_product(_req("one-touch", greeks_method="none"))
    assert r.npv == hard.npv  # the unsmoothed run's price, to the bit
    assert "second run smoothed at eps 1" in r.diagnostics
    assert not any(w.code == "greeks_hard_threshold" for w in r.warnings)


def test_joint_prices_and_differentiates_one_smoothed_run():
    r = ps.price_scripted_product(_req("ladder-call"))
    assert "price and greeks smoothed at eps 0.3" in r.diagnostics
    hard = ps.price_scripted_product(_req("ladder-call", greeks_method="none"))
    assert abs(r.npv - hard.npv) < 3 * hard.mc_std_error + 1e-9


def test_a_request_that_sets_fuzzy_is_priced_as_it_asks():
    r = ps.price_scripted_product(_req("one-touch", fuzzy=False))
    assert "smoothed at eps" not in r.diagnostics
    assert any(w.code == "greeks_hard_threshold" for w in r.warnings)


@pytest.mark.parametrize("slug", ["one-touch"])
def test_the_smoothed_greeks_see_the_jump(slug):
    # The library's levels are fixed on the first date, so under flat
    # Black-Scholes a spot bump only rescales the path: the delta is the
    # price over the spot. The vega, which moves paths across the levels,
    # is what the hard adjoint misses; the smoothed one has it, and it
    # agrees with a bump of the unsmoothed price on the same draws.
    r = ps.price_scripted_product(_req(slug, n_paths=50_000))
    up = ps.price_scripted_product(
        _req(slug, vol=0.26, greeks_method="none", n_paths=50_000)
    )
    dn = ps.price_scripted_product(
        _req(slug, vol=0.24, greeks_method="none", n_paths=50_000)
    )
    bump = (up.npv - dn.npv) / 0.02
    hard = ps.price_scripted_product(_req(slug, fuzzy=False, n_paths=50_000))
    # the width was chosen on the delta: the vega keeps a smoothing bias of a
    # few percent, far less than what the unsmoothed adjoint misses
    assert r.greeks.vega == pytest.approx(bump, rel=0.15)
    assert abs(r.greeks.vega - bump) < 0.5 * abs(hard.greeks.vega - bump)


def test_a_knock_out_products_conditioned_vega_is_the_bumped_prices():
    # The up-and-out call on knockOutAbove (#369): under Black-Scholes the
    # model conditions each close, so the adjoint's vega needs no smoothing.
    # Against a central bump of the vol by +-0.01 on the same draws (its own
    # truncation error a few percent: a barrier's vega is curved).
    r = ps.price_scripted_product(_req("up-and-out-call", n_paths=50_000))
    up = ps.price_scripted_product(
        _req("up-and-out-call", vol=0.26, greeks_method="none", n_paths=50_000)
    )
    dn = ps.price_scripted_product(
        _req("up-and-out-call", vol=0.24, greeks_method="none", n_paths=50_000)
    )
    assert "smoothed" not in r.diagnostics
    assert r.greeks.vega == pytest.approx((up.npv - dn.npv) / 0.02, rel=0.1)
