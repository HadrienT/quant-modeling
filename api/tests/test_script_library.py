"""The product library (api/app/product_library/*.qms) and its term sheets
(product_templates.py): every product passes the real parser with its default
terms, declares what the site shows, and prices. Terms move prices the way
they must; a few orderings between products are checked as properties."""

import json
import math
import re
from datetime import date, timedelta
from pathlib import Path

import pytest
import quantmodeling as qm

from app import pricing_service, product_templates as pt
from app.schemas import ScriptedProductRequest

ROOT = Path(__file__).resolve().parents[2]
SLUGS = sorted(pt.templates())
VALUATION = date(2026, 12, 31)  # before every library date: nothing moves
CATEGORIES = {
    "Vanillas and digitals",
    "Barriers and touch options",
    "Asians, lookbacks and ladders",
    "Cliquets",
    "Structured notes",
    "Volatility",
    "Multi-asset",
}


def price(script: str, n: int, *, rho=0.5, seed=1, paths=4000):
    multi = (
        dict(
            spots=[100.0] * n,
            dividends=[0.0] * n,
            vols=[0.25] * n,
            correlation=[1.0 if i == j else rho for i in range(n) for j in range(n)],
        )
        if n > 1
        else {}
    )
    return qm.price_script(
        script,
        100.0,
        0.03,
        0.0,
        0.25,
        VALUATION.isoformat(),
        n_paths=paths,
        seed=seed,
        **multi,
    )


def npv(slug, terms=None, **kw):
    t = pt.get(slug)
    return price(pt.render(slug, terms or {}, VALUATION), t.underlyings, **kw)["npv"]


def test_the_library_is_there():
    assert len(SLUGS) >= 40


@pytest.mark.parametrize("slug", SLUGS)
def test_every_product_declares_what_the_site_shows(slug):
    t = pt.get(slug)
    assert t.title and t.summary and t.sources
    assert t.category in CATEGORIES
    assert all(ord(c) < 128 for c in t.text), "plain ASCII, in English"
    for p in t.params:
        assert p.label and p.unit in pt.UNITS


@pytest.mark.parametrize("slug", SLUGS)
def test_every_product_parses_and_prices_at_its_default_terms(slug):
    t = pt.get(slug)
    script = pt.render(slug, {}, VALUATION)
    parsed = qm.validate_script(script, VALUATION.isoformat())
    assert parsed["analysis"]["n_underlyings"] == t.underlyings
    # Literal event dates are never moved, so they must be business days;
    # schedule() bounds are rolled by their convention.
    literal = "\n".join(ln for ln in script.splitlines() if "schedule(" not in ln)
    for iso in re.findall(r"\b\d{4}-\d{2}-\d{2}\b", literal):
        assert date.fromisoformat(iso).weekday() < 5, f"{iso} is not a weekday"
    res = price(script, t.underlyings)
    assert math.isfinite(res["npv"]) and math.isfinite(res["mc_std_error"])


def test_the_generated_json_is_in_sync():
    path = ROOT / "web/src/shared/products/scripted.gen.json"
    assert json.loads(path.read_text()) == json.loads(
        json.dumps(pt.catalog())
    ), "run: python scripts/gen_product_library.py"


# ── term sheets ─────────────────────────────────────────────────────────────


def test_terms_replace_the_defaults_and_nothing_else():
    script = pt.render("barrier-reverse-convertible", {"barrier": 0.55}, VALUATION)
    assert "    barrier = 0.55\n" in script
    assert "    coupon = 0.025\n" in script  # untouched default
    assert script.count("barrier = ") == 1


def test_an_unknown_term_is_an_error_naming_the_known_ones():
    with pytest.raises(ValueError, match="coupon"):
        pt.render("reverse-convertible", {"barier": 0.6}, VALUATION)


def test_dates_start_a_week_after_the_valuation_date_on_the_same_weekdays():
    today = date(2026, 9, 25)
    iso = re.compile(r"\d{4}-\d{2}-\d{2}")
    moved = [
        date.fromisoformat(d)
        for d in iso.findall(pt.render("phoenix-autocall", {}, today))
    ]
    original = [
        date.fromisoformat(d) for d in iso.findall(pt.get("phoenix-autocall").script)
    ]
    assert timedelta(days=7) <= moved[0] - today < timedelta(days=14)
    shifts = {(a - b).days for a, b in zip(moved, original)}
    assert len(shifts) == 1 and shifts.pop() % 7 == 0


def test_a_farther_knock_out_barrier_is_worth_more():
    kw = dict(seed=2, paths=20_000)
    low = npv("up-and-out-call", {"barrier": 1.15}, **kw)
    high = npv("up-and-out-call", {"barrier": 1.40}, **kw)
    assert low < high < npv("european-call", **kw)


def test_a_higher_coupon_is_worth_more_to_the_investor():
    kw = dict(seed=2, paths=20_000)
    assert npv("worst-of-autocall", {"coupon": 0.02}, **kw) < npv(
        "worst-of-autocall", {"coupon": 0.03}, **kw
    )


# ── orderings between products ─────────────────────────────────────────────


def test_the_at_the_money_call_matches_black_scholes():
    # Struck at the money on 2027-01-04, four days after the valuation date:
    # with no dividend its value is S0 x BS(1, 1, tau), tau the option's life.
    res = price(pt.render("european-call", {}, VALUATION), 1, paths=100_000)
    s, r = 0.25, 0.03
    tau = (date(2028, 1, 4) - date(2027, 1, 4)).days / 365
    d1 = (r + s * s / 2) * tau / (s * math.sqrt(tau))
    n = lambda x: 0.5 * math.erfc(-x / math.sqrt(2))  # noqa: E731
    bs = 100 * (n(d1) - math.exp(-r * tau) * n(d1 - s * math.sqrt(tau)))
    assert res["npv"] == pytest.approx(bs, abs=4 * res["mc_std_error"] + 0.05)


def test_worst_of_basket_and_best_of_are_ordered():
    kw = dict(seed=3, paths=20_000)
    assert (
        npv("worst-of-call", **kw)
        < npv("basket-call", **kw)
        < npv("best-of-call", **kw)
    )


def test_the_variance_swap_is_worth_the_variance_spread():
    # 25 % vol against a 20 % strike, discounted, up to the 252 / n
    # annualisation convention (about +1 %).
    expected = 10000 * (0.0625 - 0.04) * math.exp(-0.03)
    assert npv("variance-swap", paths=20_000) == pytest.approx(expected, rel=0.06)


# ── the service ─────────────────────────────────────────────────────────────


def test_the_service_prices_a_term_sheet_and_returns_its_script():
    resp = pricing_service.price_scripted_product(
        ScriptedProductRequest(
            product="shark-fin",
            terms={"barrier": 1.3},
            rate=0.03,
            spot=100.0,
            vol=0.25,
            valuation_date=VALUATION,
            n_paths=4000,
        )
    )
    assert "    barrier = 1.3\n" in resp.script
    assert resp.model_choice.model == "black_scholes" and resp.npv > 900
