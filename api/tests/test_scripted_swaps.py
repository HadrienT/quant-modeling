"""The swap and the swaptions of the script library (issue 313, lot b2b):
priced on the USD SOFR curve of the traded swaps, by Hull-White calibrated
to the traded swaptions -- Monte-Carlo on the script, and the same model in
closed form (engine='analytic') or on the lattice as its oracles.

Properties: the swap is the curve's value of its legs, zero at its forward
rate, and its Monte-Carlo agrees; a payer minus a receiver is the forward
swap (parity); the European's Monte-Carlo agrees with the closed form; the
Bermudan is worth at least each European it holds and its Longstaff-Schwartz
price sits just under the lattice; a payer swap gains when rates rise;
calibration recovers the parameters that generated the vols.
"""

import math
from datetime import date

import pytest

import quantmodeling as qm
from app import pricing_service as ps
from app import rates_derivatives as rd
from app.schemas import ScriptedProductRequest

SNAP = date(2026, 11, 17)
QUOTES = (
    (1.0, 0.0459),
    (2.0, 0.0466),
    (3.0, 0.0465),
    (5.0, 0.0462),
    (7.0, 0.0464),
    (10.0, 0.0471),
    (15.0, 0.0484),
    (30.0, 0.0481),
)
A, SIGMA = 0.05, 0.011


def _market(a=A, sigma=SIGMA, quotes=QUOTES):
    times, dfs = rd.sofr_curve(quotes)
    return rd.SofrHullWhite(
        as_of=SNAP,
        window_start=date(2026, 11, 7),
        swap_quotes=tuple(quotes),
        times=tuple(times),
        dfs=tuple(dfs),
        vols=((1.0, 5.0, 0.0108, 14), (5.0, 5.0, 0.0100, 19)),
        trades_used=33,
        mean_reversion=a,
        sigma=sigma,
        rmse_bp=1.0,
        worst_bp=2.0,
        converged=True,
        seconds=0.0,
    )


@pytest.fixture
def market(monkeypatch):
    m = _market()
    monkeypatch.setattr(rd, "sofr_hull_white", lambda today=None: m)
    return m


def _req(product, **kw):
    return ScriptedProductRequest(product=product, valuation_date=SNAP, **kw)


def _periods(product, terms=None):
    from app import product_templates

    script = product_templates.render(product, terms or {}, SNAP)
    return ps._swap_periods(script, SNAP, "ACT/365F")


def test_the_swap_is_its_legs_on_the_curve(market):
    r = ps.price_scripted_product(_req("interest-rate-swap", engine="analytic"))
    periods = _periods("interest-rate-swap")
    ends = [e for _, e, _ in periods]
    p = qm.rate_curve_discount_factors(
        list(market.times), list(market.dfs), [periods[0][0]] + ends
    )
    annuity = sum(acc * pe for (_, _, acc), pe in zip(periods, p[1:]))
    exact = 10_000_000 * (p[0] - p[-1] - 0.046 * annuity)
    assert r.npv == pytest.approx(exact, rel=1e-10)
    assert r.model_choice.rates_calibration.mean_reversion == A
    assert r.model_choice.code == "swaption_calibrated"


def test_the_swap_is_worth_nothing_at_its_forward_rate(market):
    periods = _periods("interest-rate-swap")
    forward = qm.hull_white_swap_on_dates(
        list(market.times), list(market.dfs), A, SIGMA, periods, 0.0
    )["forward"]
    r = ps.price_scripted_product(
        _req("interest-rate-swap", engine="analytic", terms={"fixed_rate": forward})
    )
    assert abs(r.npv) < 1e-6


def test_the_swaps_monte_carlo_agrees_and_needs_no_volatility(market):
    exact = ps.price_scripted_product(_req("interest-rate-swap", engine="analytic"))
    flat = ps.price_scripted_product(
        _req("interest-rate-swap", hull_white={"sigma": 1e-12}, n_paths=1_000)
    )
    assert flat.npv == pytest.approx(exact.npv, rel=1e-8)
    mc = ps.price_scripted_product(_req("interest-rate-swap", n_paths=100_000, seed=5))
    assert "a=0.050000 sigma=0.011000" in mc.diagnostics  # the calibrated ones
    assert abs(mc.npv - exact.npv) < 4 * mc.mc_std_error


def test_payer_minus_receiver_is_the_forward_swap(market):
    def price(side, **kw):
        return ps.price_scripted_product(
            _req("european-swaption", terms={"side": side, "strike": 0.045}, **kw)
        )

    swap = qm.hull_white_swap_on_dates(
        list(market.times),
        list(market.dfs),
        A,
        SIGMA,
        _periods("european-swaption"),
        0.045,
        True,
        10_000_000,
    )["swap"]
    payer, receiver = price(1, engine="analytic"), price(-1, engine="analytic")
    assert payer.npv - receiver.npv == pytest.approx(swap, rel=1e-8)
    mc_p, mc_r = price(1, n_paths=100_000, seed=2), price(-1, n_paths=100_000, seed=2)
    se = math.hypot(mc_p.mc_std_error, mc_r.mc_std_error)
    assert abs((mc_p.npv - mc_r.npv) - swap) < 4 * se + 1e-6 * abs(swap)


@pytest.mark.parametrize("side", [1, -1])
def test_the_european_monte_carlo_agrees_with_the_closed_form(market, side):
    terms = {"side": side}
    exact = ps.price_scripted_product(
        _req("european-swaption", engine="analytic", terms=terms)
    )
    mc = ps.price_scripted_product(
        _req("european-swaption", terms=terms, n_paths=100_000, seed=7)
    )
    assert exact.npv > 0
    assert abs(mc.npv - exact.npv) < 4 * mc.mc_std_error
    assert exact.greeks.vega > 0  # a swaption gains with the rate's vol


def test_the_bermudan_sits_under_the_lattice_and_above_each_european(market):
    periods = _periods("bermudan-swaption")
    exercises = [s for s, _, _ in periods]
    oracle = qm.hull_white_swap_on_dates(
        list(market.times),
        list(market.dfs),
        A,
        SIGMA,
        periods,
        0.046,
        True,
        10_000_000,
        exercises,
    )
    lattice, first = oracle["bermudan"], oracle["european"]
    lsm = ps.price_scripted_product(_req("bermudan-swaption", n_paths=100_000, seed=3))
    assert lsm.npv < lattice + 3 * lsm.mc_std_error
    assert lsm.npv > 0.985 * lattice - 3 * lsm.mc_std_error
    assert lattice > first
    # every co-terminal European is in the Bermudan
    for k, t in enumerate(exercises):
        e = qm.hull_white_swap_on_dates(
            list(market.times),
            list(market.dfs),
            A,
            SIGMA,
            periods[k:],
            0.046,
            True,
            10_000_000,
            [t],
        )["european"]
        assert lattice >= e


def test_a_payer_swap_gains_when_rates_rise(market):
    r = ps.price_scripted_product(_req("interest-rate-swap", engine="analytic"))
    periods = _periods("interest-rate-swap")
    annuity = qm.hull_white_swap_on_dates(
        list(market.times), list(market.dfs), A, SIGMA, periods, 0.046
    )["annuity"]
    dv01 = r.greeks.rho * 1e-4
    # close to the annuity's 1bp: the curve moves by about the quotes' move
    assert dv01 == pytest.approx(annuity * 10_000_000 * 1e-4, rel=0.05)
    receiver = ps.price_scripted_product(
        _req("interest-rate-swap", engine="analytic", terms={"side": -1})
    )
    assert receiver.greeks.rho == pytest.approx(-r.greeks.rho, rel=1e-9)


def test_a_swap_takes_no_other_curve():
    with pytest.raises(ValueError, match="SOFR"):
        _req("interest-rate-swap", hull_white={"currency": "EUR"})
    with pytest.raises(ValueError, match="SOFR"):
        _req("european-swaption", hull_white={"curve": "government"})
    with pytest.raises(ValueError, match="rates product"):
        _req("bermudan-swaption", ticker="AAPL")
    assert _req("interest-rate-swap").hull_white.curve == "sofr"


def test_calibration_recovers_the_parameters_behind_the_vols(monkeypatch):
    # vols the model itself gives at (a, sigma): calibrating to them returns it
    times, dfs = rd.sofr_curve(QUOTES)
    grid = [(1.0, 5.0), (1.0, 10.0), (2.0, 5.0), (5.0, 5.0), (5.0, 10.0), (10.0, 10.0)]
    vols = []
    for x, t in grid:
        out = qm.price_swaption(
            times, dfs, times, dfs, x, t, None, True, 1, 1, hull_white=(A, SIGMA)
        )
        vols.append(
            {
                "expiry": x,
                "tenor": t,
                "normal_vol": out["hull_white_normal_vol"],
                "trades": 9,
            }
        )
    monkeypatch.setattr(
        rd,
        "market_request",
        lambda today=None: {
            "as_of": SNAP,
            "window_start": date(2026, 11, 7),
            "ois": [{"tenor": t, "rate": r} for t, r in QUOTES],
            "market_vols": vols,
            "trades_used": 54,
        },
    )
    rd._sofr_cache.clear()
    m = rd.sofr_hull_white(SNAP)
    rd._sofr_cache.clear()
    assert m.mean_reversion == pytest.approx(A, abs=2e-3)
    assert m.sigma == pytest.approx(SIGMA, rel=2e-2)
    assert m.rmse_bp < 0.5


# ── adjoint risks to the curve and to sigma (Monte-Carlo) ────────────────────


@pytest.mark.parametrize("product", ["interest-rate-swap", "european-swaption"])
def test_the_adjoint_dv01_and_vega_are_the_closed_forms(market, product):
    exact = ps.price_scripted_product(_req(product, engine="analytic"))
    mc = ps.price_scripted_product(
        _req(product, n_paths=100_000, seed=3, greeks_method="aad")
    )
    g = mc.greeks
    # the closed form's DV01 is a central difference: allow its O(h^2) too
    assert abs(g.rho - exact.greeks.rho) < 4 * g.rho_std_error + 1e-4 * abs(
        exact.greeks.rho
    )
    if product == "european-swaption":
        assert abs(g.vega - exact.greeks.vega) < 4 * g.vega_std_error + 1e-4 * abs(
            exact.greeks.vega
        )
    labels = [r.label for r in mc.risks]
    assert "spot" not in labels and "vol" not in labels  # the unread stock
    assert labels[0] == "rate_vol" and labels[-1] == "curve_parallel"


def test_the_quote_risks_add_up_to_the_parallel_one(market):
    mc = ps.price_scripted_product(
        _req("bermudan-swaption", n_paths=20_000, seed=1, greeks_method="aad")
    )
    by = {r.label: r.value for r in mc.risks}
    buckets = [v for k, v in by.items() if k.startswith("swap_rate[")]
    assert len(buckets) == len(QUOTES)
    assert sum(buckets) == pytest.approx(by["curve_parallel"], rel=1e-9)
    assert by["swap_rate[30]"] == 0.0  # beyond the swap: nothing moves it
    assert by["rate_vol"] > 0  # a Bermudan gains with the rate's vol
