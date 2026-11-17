"""A library product's numbers are its terms, read as data by the GPU's
compiled kernel (one kernel per product, whatever the term sheet); a script
a user wrote keeps its numbers as constants."""

from app import pricing_service as ps
from app.schemas import ScriptedProductRequest, ScriptRequest


def _capture(monkeypatch):
    calls = []
    real = ps.qm.price_script

    def spy(*a, **kw):
        calls.append(kw)
        return real(*a, **kw)

    monkeypatch.setattr(ps.qm, "price_script", spy)
    return calls


def test_a_library_product_prices_with_its_terms_as_data(monkeypatch):
    calls = _capture(monkeypatch)
    common = dict(spot=100.0, vol=0.2, rate=0.03, model="black_scholes", n_paths=2000)
    ps.price_scripted_product(
        ScriptedProductRequest(product="asian-call", terms={}, **common)
    )
    assert calls[-1]["terms_as_parameters"] is True
    ps.price_script(
        ScriptRequest(script="2027-06-01\n    pays max(spot() - 100, 0)\n", **common)
    )
    assert calls[-1]["terms_as_parameters"] is False


def test_the_same_price_either_way():
    # Terms as data change only how the GPU's kernel reads them: same price.
    from app import product_templates
    from datetime import date

    script = product_templates.render("asian-call", {}, date(2026, 6, 1))
    args = (script, 100.0, 0.03, 0.0, 0.2, "2026-06-01")
    a = ps.qm.price_script(*args, n_paths=4000, terms_as_parameters=True)
    b = ps.qm.price_script(*args, n_paths=4000, terms_as_parameters=False)
    assert a["npv"] == b["npv"]
