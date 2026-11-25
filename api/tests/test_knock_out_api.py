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


def test_a_knock_out_prices_on_local_vol_with_a_smooth_delta(store):  # noqa: F811
    soft = pricing_service.price_script(
        _req(_script(True), ticker="TEST", greeks_method="aad", n_paths=20000)
    )
    assert soft.model_choice.model == "local_vol"
    assert soft.model_choice.code == "barrier_monitoring"
    codes = {w.code for w in soft.warnings}
    assert "greeks_hard_threshold" not in codes
    assert soft.greeks.delta is not None
    assert soft.greeks.delta > 3 * soft.greeks.delta_std_error

    # The same model: under 'auto' the hard daily barrier goes to SLV (a
    # path-dependent payoff, the forward smile), the knock-out to local vol
    # (the models that condition their steps: Black-Scholes and local vol).
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


def test_a_knock_out_under_heston_is_refused_with_the_reason(store):  # noqa: F811
    import pytest

    with pytest.raises(Exception, match="knockOutAbove"):
        pricing_service.price_script(
            _req(_script(True), ticker="TEST", model="heston", device="cpu")
        )
