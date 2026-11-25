"""knockOutAbove / knockOutBelow through the API (issue #369): model 'auto'
sends it to local vol, the multilevel default prices it (on the CPU: the
conditioned step is not in the GPU's IR yet), its price agrees with the hard
script's, and its adjoint delta comes with no smoothing and no warning. The
native module prices for real; the database is the synthetic surface of
test_script_model_choice."""

from app import pricing_service

from test_script_model_choice import _req, store  # noqa: F401 (fixture)

DATES = [
    "2026-10-12",
    "2026-11-11",
    "2026-12-11",
    "2027-01-11",
    "2027-02-11",
    "2027-03-11",
]


def _script(conditioned: bool) -> str:
    lines = [] if conditioned else ["2026-09-14", "    alive = 1"]
    for d in DATES:
        lines.append(d)
        lines.append(
            "    knockOutAbove(120)"
            if conditioned
            else "    if spot() >= 120 then alive = 0 endIf"
        )
    lines.append(
        "    pays max(spot() - 100, 0)"
        if conditioned
        else "    if alive = 1 then pays max(spot() - 100, 0) endIf"
    )
    return "\n".join(lines) + "\n"


def test_a_knock_out_under_local_vol_has_a_smooth_delta(store):  # noqa: F811
    soft = pricing_service.price_script(
        _req(
            _script(True),
            ticker="TEST",
            model="local_vol",
            greeks_method="aad",
            n_paths=20000,
        )
    )
    codes = {w.code for w in soft.warnings}
    assert "greeks_hard_threshold" not in codes
    assert soft.greeks.delta is not None
    assert soft.greeks.delta > 3 * soft.greeks.delta_std_error

    hard = pricing_service.price_script(
        _req(
            _script(False),
            ticker="TEST",
            model="local_vol",
            greeks_method="none",
            n_paths=20000,
        )
    )
    err = (soft.mc_std_error**2 + hard.mc_std_error**2) ** 0.5
    assert abs(soft.npv - hard.npv) < 4 * err


def test_auto_prices_a_knock_out_on_the_model_of_the_hard_barrier(store):  # noqa: F811
    # A barrier on dates is path-dependent: 'auto' picks SLV for both
    # writings, and they agree on it. The synthetic fit breaks Feller, so
    # the knock-out's greeks are withheld with the reason (#369, as #372).
    soft = pricing_service.price_script(
        _req(_script(True), ticker="TEST", greeks_method="aad", n_paths=20000)
    )
    hard = pricing_service.price_script(
        _req(_script(False), ticker="TEST", greeks_method="none", n_paths=20000)
    )
    assert soft.model_choice.model == hard.model_choice.model == "slv"
    err = (soft.mc_std_error**2 + hard.mc_std_error**2) ** 0.5
    assert abs(soft.npv - hard.npv) < 4 * err
    assert "last_step_digital_greeks_withheld" in {w.code for w in soft.warnings}
    assert not soft.risks and soft.greeks.delta is None


# ── the library's knock-out products (#369 L2) ─────────────────────────────


def _library(product: str, model: str, **kw):
    from app.schemas import ScriptedProductRequest

    from test_script_model_choice import SNAP

    base = dict(
        product=product,
        model=model,
        ticker="TEST",
        rate=0.03,
        valuation_date=SNAP,
        n_paths=4000,
        seed=3,
        device="cpu",
        greeks_method="aad",
        maturity=0.5,
    )
    return pricing_service.price_scripted_product(
        ScriptedProductRequest(**{**base, **kw})
    )


def _hard(response, model: str):
    """The same product as the hard script it replaces: its thresholds."""
    from app.schemas import ScriptRequest

    from test_script_model_choice import SNAP

    return pricing_service.price_script(
        ScriptRequest(
            script=response.script,
            model=model,
            ticker="TEST",
            rate=0.03,
            valuation_date=SNAP,
            n_paths=4000,
            seed=5,
            device="cpu",
            greeks_method="none",
            knock_outs_as_thresholds=True,
        ),
        library=True,
        start_today=True,
    )


import pytest  # noqa: E402


@pytest.mark.parametrize(
    "product", ["up-and-out-call", "double-barrier-call", "double-no-touch"]
)
def test_a_library_knock_out_under_local_vol_conditions_with_its_own_greeks(
    store, product
):  # noqa: F811
    r = _library(product, "local_vol")
    assert "smoothed" not in r.diagnostics
    assert (
        r.greeks.delta is not None
        and abs(r.greeks.delta) > 2 * r.greeks.delta_std_error
    )
    assert "greeks_hard_threshold" not in {w.code for w in r.warnings}
    hard = _hard(r, "local_vol")
    assert abs(r.npv - hard.npv) < 4 * (r.mc_std_error**2 + hard.mc_std_error**2) ** 0.5


def test_a_library_knock_out_under_heston_takes_smoothed_greeks(store):  # noqa: F811
    r = _library("up-and-out-call", "heston")
    assert "thresholds smoothed at eps 3" in r.diagnostics
    assert r.greeks.delta is not None and r.risks
    # the price is the conditioned run's, the hard script's within error
    hard = _hard(r, "heston")
    assert abs(r.npv - hard.npv) < 4 * (r.mc_std_error**2 + hard.mc_std_error**2) ** 0.5


def test_auto_gives_a_library_knock_out_slv_and_smoothed_greeks(store):  # noqa: F811
    r = _library("up-and-out-call", "auto")
    assert r.model_choice.model == "slv"
    assert "thresholds smoothed" in r.diagnostics
    assert r.greeks.delta is not None


# ── the shark-fin's rebate as signed parts (#369 L2b) ───────────────────────

# Only the shark-fin: its unconditioned part is a constant (the rebate). For
# the knock-ins, measured, the two runs' variances add up to more than the
# hard script's -- they stay on it (WP 26 §26.4, L2b).
PART_PRODUCTS = ["shark-fin"]


def _main_script(product: str, model: str, n_paths: int = 8000):
    """The product as its one hard script, as every other use reads it."""
    from app import product_templates as pt
    from app.schemas import ScriptRequest

    from test_script_model_choice import SNAP

    return pricing_service.price_script(
        ScriptRequest(
            script=pt.render(product, {}, SNAP, maturity=0.5),
            model=model,
            ticker="TEST",
            rate=0.03,
            valuation_date=SNAP,
            n_paths=n_paths,
            seed=11,
            device="cpu",
            greeks_method="none",
        ),
        library=True,
        start_today=True,
    )


@pytest.mark.parametrize("product", PART_PRODUCTS)
@pytest.mark.parametrize("model", ["local_vol", "slv"])
def test_signed_parts_price_the_hard_script(store, product, model):  # noqa: F811
    r = _library(product, model, n_paths=8000)
    assert r.diagnostics.startswith("signed parts")
    assert r.model_choice.model == model
    assert r.script.count("# part: ") == 2
    hard = _main_script(product, model)
    err = (r.mc_std_error**2 + hard.mc_std_error**2) ** 0.5
    assert abs(r.npv - hard.npv) < 4 * err, (r.npv, hard.npv, err)
    assert r.greeks.delta is not None


def test_auto_prices_every_part_under_the_products_model(store):  # noqa: F811
    r = _library("shark-fin", "auto")
    assert r.model_choice.model == "slv" and r.model_choice.requested == "auto"
    assert r.model_choice.code != "user"
    assert r.diagnostics.count("model slv") >= 2


def test_parts_are_read_with_their_signs_and_must_span_the_scripts_dates():
    from app import product_templates as pt

    t = pt.get("shark-fin")
    assert [s for s, _ in t.parts] == [1, 1]
    assert "knockOutAbove" in t.parts[1][1] and "knockOut" not in t.script
    bad = t.text.replace(
        "2028-01-04\n    pays notional * (1 + rebate)",
        "2027-12-29\n    pays notional * (1 + rebate)",
    )
    with pytest.raises(ValueError, match="first and last dates"):
        pt.parse("shark-fin", bad)
