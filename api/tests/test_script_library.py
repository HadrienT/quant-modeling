"""The product library of the scripting page (web/.../scripting/library/*.qms):
every script passes the real parser, declares what the page shows about it,
and prices. A few orderings between products are checked as properties, the
way the rest of the test suite checks engines."""

import math
import re
from datetime import date
from pathlib import Path

import pytest
import quantmodeling as qm

LIBRARY = (
    Path(__file__).resolve().parents[2] / "web/src/features/pricing/scripting/library"
)
FILES = sorted(LIBRARY.glob("*.qms"))
VALUATION = "2026-12-31"  # before every library date, as the page shifts them
CATEGORIES = {
    "Vanillas and digitals",
    "Barriers and touch options",
    "Asians, lookbacks and ladders",
    "Cliquets",
    "Structured notes",
    "Volatility",
    "Multi-asset",
}


def header(src: str) -> dict:
    meta: dict = {"source": []}
    for m in re.finditer(r"^# (\w+): (.+)$", src, re.M):
        key, value = m.group(1), m.group(2).strip()
        if key == "source":
            meta["source"].append(value)
        else:
            meta[key] = value
    return meta


def price(src: str, n: int, *, rho: float = 0.5, seed: int = 1, paths: int = 4000):
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
        src, 100.0, 0.03, 0.0, 0.25, VALUATION, n_paths=paths, seed=seed, **multi
    )


def load(slug: str) -> str:
    return (LIBRARY / f"{slug}.qms").read_text()


def test_the_library_is_there():
    assert len(FILES) >= 40


@pytest.mark.parametrize("path", FILES, ids=lambda p: p.stem)
def test_every_product_declares_what_the_page_shows(path):
    meta = header(path.read_text())
    assert meta.get("title") and meta.get("summary")
    assert meta.get("category") in CATEGORIES
    assert int(meta["underlyings"]) >= 1
    assert meta["source"], "every product cites at least one source"
    assert all(ord(c) < 128 for c in path.read_text()), "plain ASCII, in English"


@pytest.mark.parametrize("path", FILES, ids=lambda p: p.stem)
def test_every_product_parses_and_prices(path):
    src = path.read_text()
    n = int(header(src)["underlyings"])
    parsed = qm.validate_script(src, VALUATION)
    assert parsed["analysis"]["n_underlyings"] == n
    # Literal event dates are never moved, so they must be business days;
    # schedule() bounds are rolled by their convention.
    literal = "\n".join(ln for ln in src.splitlines() if "schedule(" not in ln)
    for iso in re.findall(r"\b\d{4}-\d{2}-\d{2}\b", literal):
        assert date.fromisoformat(iso).weekday() < 5, f"{iso} is not a weekday"
    res = price(src, n)
    assert math.isfinite(res["npv"]) and math.isfinite(res["mc_std_error"])


def test_the_at_the_money_call_matches_black_scholes():
    # Struck at the money on 2027-01-04, four days after the valuation date:
    # with no dividend its value is S0 x BS(1, 1, tau), tau the option's life.
    res = price(load("european-call"), 1, paths=100_000)
    s, r = 0.25, 0.03
    tau = (date(2028, 1, 4) - date(2027, 1, 4)).days / 365
    d1 = (r + s * s / 2) * tau / (s * math.sqrt(tau))
    n = lambda x: 0.5 * math.erfc(-x / math.sqrt(2))  # noqa: E731
    bs = 100 * (n(d1) - math.exp(-r * tau) * n(d1 - s * math.sqrt(tau)))
    assert res["npv"] == pytest.approx(bs, abs=4 * res["mc_std_error"] + 0.05)


def test_worst_of_basket_and_best_of_are_ordered():
    wo, basket, bo = (
        price(load(s), 3, seed=3, paths=20_000)["npv"]
        for s in ("worst-of-call", "basket-call", "best-of-call")
    )
    assert wo < basket < bo


def test_barriers_are_cheaper_than_their_vanillas():
    up_out = price(load("up-and-out-call"), 1, seed=5, paths=20_000)["npv"]
    call = price(load("european-call"), 1, seed=5, paths=20_000)["npv"]
    assert 0 < up_out < call


def test_the_variance_swap_is_worth_the_variance_spread():
    # 25 % vol against a 20 % strike: about 10,000 x (0.0625 - 0.04),
    # discounted, up to the 252 / n annualisation convention (+1 %).
    npv = price(load("variance-swap"), 1, paths=20_000)["npv"]
    expected = 10000 * (0.0625 - 0.04) * math.exp(-0.03)
    assert npv == pytest.approx(expected, rel=0.06)
