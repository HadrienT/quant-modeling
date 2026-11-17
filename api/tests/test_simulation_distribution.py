"""The simulation page's diagnostics (WP 29): the law of a model's paths, the
prices they imply, and the stored surface beside them. The native module
simulates for real; the database is a synthetic surface."""

import math

import pytest
from pydantic import ValidationError

from app import market_snapshot, simulation_market, stochastic_vol
from app.routers import simulation
from app.schemas import DistributionRequest

from test_script_model_choice import _market  # noqa: E402 (tests dir on sys.path)


@pytest.fixture
def store(monkeypatch):
    stochastic_vol._cache.clear()
    monkeypatch.setattr(market_snapshot, "local_vol_market", lambda *a: _market())
    monkeypatch.setattr(stochastic_vol, "SLV_PARTICLES", 20_000)
    yield
    stochastic_vol._cache.clear()


def _lognormal_pdf(K, F, sigma, T):
    s = sigma * math.sqrt(T)
    return math.exp(-((math.log(K / F) + 0.5 * s * s) ** 2) / (2 * s * s)) / (
        K * s * math.sqrt(2 * math.pi)
    )


def test_a_flat_svi_density_is_the_lognormal_one():
    sigma = 0.25
    slices = [
        dict(ttm=t, a=sigma * sigma * t, b=0.0, rho=0.0, m=0.0, sigma=0.1)
        for t in (0.5, 1.0)
    ]
    strikes = [60.0, 90.0, 100.0, 130.0]
    for K, q in zip(strikes, simulation_market.density(slices, 0.75, 100.0, strikes)):
        assert q == pytest.approx(_lognormal_pdf(K, 100.0, sigma, 0.75), rel=1e-12)
    assert simulation_market.implied_vols(slices, 0.75, [0.8, 1.2]) == pytest.approx(
        [sigma, sigma], rel=1e-12
    )


def test_the_market_is_only_read_where_it_is_quoted():
    slices = market_snapshot.surface_slices(_market().svi_slices)
    first, last = slices[0]["ttm"], slices[-1]["ttm"]
    assert simulation_market.implied_vols(slices, first / 2, [1.0]) == [None]
    assert simulation_market.implied_vols(slices, last * 1.5, [1.0]) == [None]
    assert simulation_market.implied_vols(slices, 1.0, [1.0])[0] > 0


def test_a_skewed_svi_density_integrates_to_one():
    slices = market_snapshot.surface_slices(_market().svi_slices)
    T, F = 1.0, 100.0 * math.exp(0.03)
    strikes = [F * math.exp(-1.5 + 3.0 * i / 3000) for i in range(3001)]
    q = simulation_market.density(slices, T, F, strikes)
    pairs = [(K, d) for K, d in zip(strikes, q) if d is not None]
    mass = sum(
        0.5 * (d0 + d1) * (K1 - K0) for (K0, d0), (K1, d1) in zip(pairs, pairs[1:])
    )
    # ln(K / F) in [-1.5, 1.5]: nearly all the mass, never more than all of it
    assert 0.99 < mass < 1.0 + 1e-6
    assert all(d >= 0 for _, d in pairs)  # no butterfly arbitrage on this surface


def test_black_scholes_typed_is_flat_and_has_no_market():
    out = simulation.distribution(
        DistributionRequest(
            model="black_scholes",
            spot=100.0,
            vol=0.2,
            rate=0.03,
            ttm=1.0,
            n_steps=12,
            n_paths=10_000,
            n_display=5,
        )
    )
    assert out.market is None and len(out.paths) == 5 and len(out.time_grid) == 13
    assert [s.time for s in out.smiles] == pytest.approx([0.25, 0.5, 1.0])
    for smile in out.smiles + [out.forward_smile]:
        assert smile.market_iv is None
        for iv, se in zip(smile.model_iv, smile.iv_stderr):
            assert iv == pytest.approx(0.2, abs=4 * se + 1e-4)
    assert out.forward_smile.start == pytest.approx(0.5)
    assert all(v == pytest.approx(0.2) for v in out.vols[0][1:])
    assert out.barrier.price < out.barrier.vanilla_price
    out.model_dump_json()  # no NaN left


@pytest.mark.parametrize(
    "model", ["black_scholes", "sabr", "local_vol", "heston", "slv"]
)
def test_every_model_from_a_ticker_is_drawn_against_the_surface(store, model):
    out = simulation.distribution(
        DistributionRequest(
            model=model, ticker="TEST", rate=0.03, ttm=1.0, n_steps=12, n_paths=2_000
        )
    )
    assert out.market.ticker == "TEST" and out.spot > 0
    last = out.smiles[-1]
    assert last.market_iv is not None and any(v is not None for v in last.market_iv)
    density = out.histogram.market_density[-1]
    assert density is not None and any(d is not None for d in density)
    # the martingale check, within 4 standard errors at every date
    mg = out.martingale
    for mean, se, exact in zip(mg.mean, mg.stderr, mg.exact):
        assert mean == pytest.approx(exact, abs=4 * se + 1e-9)
    if model == "sabr":  # a forward to ttm: only its last date is the market's
        assert out.smiles[0].market_iv is None
        assert out.histogram.market_density[1] is None
    out.model_dump_json()


def test_inputs_are_checked():
    with pytest.raises(ValidationError, match="ticker"):
        DistributionRequest(model="local_vol", spot=100.0)
    with pytest.raises(ValidationError, match="parameters"):
        DistributionRequest(model="heston", spot=100.0)
    with pytest.raises(ValidationError, match="barrier"):
        DistributionRequest(
            model="black_scholes", spot=100.0, vol=0.2, barrier=1.1, barrier_strike=1.2
        )
    with pytest.raises(ValidationError):
        DistributionRequest(model="black_scholes", spot=100.0, vol=0.2, n_paths=50_000)


def test_a_smile_with_a_call_spread_arbitrage_is_not_a_density():
    # rho at -1 and a steep left wing: P(S_T > K) above 1 at low strikes
    bad = [
        dict(ttm=t, a=0.0077 * t, b=1.42 * t, rho=-0.999, m=-0.468, sigma=0.1275)
        for t in (1.0, 1.2)
    ]
    F = 100.0
    strikes = [F * math.exp(-0.39 + 0.69 * i / 50) for i in range(51)]
    assert not simulation_market.is_density(bad, 1.0, F, strikes)
    flat = [
        dict(ttm=t, a=0.04 * t, b=0.0, rho=0.0, m=0.0, sigma=0.1) for t in (1.0, 1.2)
    ]
    assert simulation_market.is_density(flat, 1.0, F, strikes)
