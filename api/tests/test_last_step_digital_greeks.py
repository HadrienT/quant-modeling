"""lastStepDigital's greeks where they fail (#372): under Heston and SLV
whose variance breaks Feller they have infinite variance, under rough
Bergomi they read zero. A user script there gets no greeks and a warning;
a library digital takes them from the threshold smoothed at its measured
eps, its price staying lastStepDigital's. The native module prices for real;
the database is the synthetic surface of test_script_model_choice."""

import pytest

from app import pricing_service, product_templates as pt
from app.schemas import ScriptedProductRequest

from test_script_model_choice import SNAP, _req, store  # noqa: F401 (fixture)


def test_the_rewrite_smooths_each_last_step_digital():
    assert pt.smoothed_last_step("    pays 100 * lastStepDigital(1.05 * s0)\n", 3) == (
        "    pays 100 * smooth(spot() - (1.05 * s0), 3)\n"
    )
    assert pt.smoothed_last_step(
        "pays (1 - lastStepDigital(max(k, 2), 1)) + lastStepDigital(k)", 0.5
    ) == ("pays (1 - smooth(spot(1) - (max(k, 2)), 0.5)) + smooth(spot() - (k), 0.5)")
    assert (
        pt.smoothed_last_step("pays lastStepCall(100)", 3) == "pays lastStepCall(100)"
    )
    # comments are not code
    kept = "# lastStepDigital(K, as written\n"
    assert pt.smoothed_last_step(kept, 3) == kept


def test_the_smoothed_scripts_of_the_library_parse():
    import quantmodeling as qm

    for slug in ("digital-call", "digital-put"):
        t = pt.get(slug)
        assert t.greeks == "last-step" and t.eps == 3
        script = pt.render(slug, {}, SNAP)
        smoothed = pt.smoothed_last_step(script, t.eps)
        code = "".join(line.partition("#")[0] for line in smoothed.splitlines())
        assert "lastStepDigital" not in code and "smooth(spot() - (" in code
        assert (
            qm.validate_script(smoothed, SNAP.isoformat(), "ACT/365F")["analysis"][
                "spot_comparison"
            ]
            is False
        )


def test_last_step_needs_eps_and_last_step_digital():
    text = pt.get("digital-call").text.replace("# eps: 3\n", "")
    with pytest.raises(ValueError, match="last-step"):
        pt.parse("digital-call", text)


def _digital(model: str, **kw):
    base = dict(
        product="digital-call",
        model=model,
        ticker="TEST",
        rate=0.03,
        valuation_date=SNAP,
        n_paths=4000,
        seed=3,
        device="cpu",
        greeks_method="aad",
    )
    return pricing_service.price_scripted_product(
        ScriptedProductRequest(**{**base, **kw})
    )


def test_a_library_digital_under_heston_takes_smoothed_greeks(store):  # noqa: F811
    r = _digital("heston")
    assert "threshold smoothed at eps 3" in r.diagnostics
    assert r.greeks.delta is not None and r.greeks.delta > 2 * r.greeks.delta_std_error
    codes = {w.code for w in r.warnings}
    assert "last_step_digital_greeks_withheld" not in codes
    assert "greeks_hard_threshold" not in codes
    # the price is lastStepDigital's, not the smoothed run's
    assert r.npv == _digital("heston", greeks_method="none").npv


def test_a_library_digital_under_local_vol_keeps_its_own_greeks(store):  # noqa: F811
    r = _digital("local_vol", mlmc="off")
    assert "smoothed" not in r.diagnostics
    assert r.greeks.delta is not None and r.greeks.delta > 2 * r.greeks.delta_std_error


def test_a_user_script_on_last_step_digital_under_heston_gets_no_greeks(
    store,
):  # noqa: F811
    r = pricing_service.price_script(
        _req(
            "2027-09-13\n    pays 100 * lastStepDigital(100)\n",
            model="heston",
            ticker="TEST",
            device="cpu",
            greeks_method="aad",
        )
    )
    h = r.model_choice.calibration.heston
    if 2 * h.kappa * h.theta / h.xi**2 >= 1:
        pytest.skip("the synthetic surface's Heston fit satisfies Feller")
    assert "last_step_digital_greeks_withheld" in {w.code for w in r.warnings}
    assert not r.risks and r.greeks.delta is None
    assert r.npv > 0
