"""Heston and SLV price by QE, differentiate by Euler (#317): with AAD greeks
the API runs both and merges them, saying so; without greeks, one QE run."""

from app import pricing_service as ps


def _fake_run(calls):
    def run(greeks_method, second_order, scheme, **over):
        calls.append((greeks_method, second_order, scheme, over))
        if greeks_method == "aad":
            return {
                "npv": 10.3,
                "mc_std_error": 0.05,
                "greeks": {"delta": 0.5},
                "risks": [{"label": "spot", "value": 0.5, "std_error": 0.01}],
                "warnings": [{"code": "a", "severity": "info", "message": "m"}],
                "diagnostics": "adjoint",
            }
        return {
            "npv": 10.0,
            "mc_std_error": 0.04,
            "discretisation_bias": 0.002,
            "greeks": {},
            "warnings": [{"code": "a", "severity": "info", "message": "m"}],
            "diagnostics": "price",
        }

    return run


def test_without_greeks_one_qe_run():
    calls = []
    r = ps._qe_price_euler_risks(_fake_run(calls), "none", False)
    assert [c[:3] for c in calls] == [("none", False, "qe")]
    assert r["npv"] == 10.0


def test_aad_greeks_take_the_qe_price_and_the_euler_risks():
    calls = []
    r = ps._qe_price_euler_risks(_fake_run(calls), "aad", True)
    assert [c[:3] for c in calls] == [("none", False, "qe"), ("aad", True, "euler")]
    assert calls[1][3] == {"bias_estimate": False}
    # the price, its error and its bias from the QE run; the greeks from the adjoint
    assert (r["npv"], r["mc_std_error"], r["discretisation_bias"]) == (
        10.0,
        0.04,
        0.002,
    )
    assert r["greeks"] == {"delta": 0.5} and r["risks"][0]["label"] == "spot"
    assert len(r["warnings"]) == 1  # not repeated
    assert "Euler" in r["diagnostics"] and "QE" in r["diagnostics"]
