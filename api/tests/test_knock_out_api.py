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
