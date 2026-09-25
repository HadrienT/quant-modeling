"""model='auto' on the scripting endpoint: the model the script needs is
chosen, announced and recorded, and the stochastic-vol calibration it may
need is run once per snapshot. The real native module prices and calibrates;
only the database is replaced, by a synthetic market surface."""

import math
from datetime import date

import pytest
from pydantic import ValidationError

from app import market_snapshot, pricing_service, stochastic_vol, valuation
from app.schemas import ScriptRequest

SNAP = date(2026, 9, 11)

VANILLA = "2027-09-13\n    pays max(spot() - 100, 0)\n"
BARRIER = (
    "2027-03-11\n    alive = 1\n    if spot() < 80 then alive = 0 endIf\n"
    "2027-09-13\n    if spot() < 80 then alive = 0 endIf\n"
    "    if alive = 1 then pays max(spot() - 100, 0) endIf\n"
)


def _svi(T: float) -> dict:
    """Raw SVI with a 20 % ATM vol and an equity put skew."""
    return dict(ttm=T, a=0.03 * T, b=0.1 * T, rho=-0.6, m=0.0, sigma=0.1)


def _market() -> market_snapshot.LocalVolMarket:
    K = [50.0 + 5.0 * i for i in range(21)]
    T = [0.1, 0.5, 1.0, 2.0]
    sigma = [0.2 + 0.3 * max(0.0, (100.0 - k) / 100.0) for k in K for _ in T]
    return market_snapshot.LocalVolMarket(
        ticker="TEST",
        valuation_date=SNAP,
        spot=100.0,
        dividend=0.0,
        K_grid=K,
        T_grid=T,
        sigma_loc_flat=sigma,
        svi_slices=[_svi(t) for t in (0.02, 0.1, 0.25, 0.5, 1.0, 2.0)],
        rate=0.03,
    )


@pytest.fixture
def store(monkeypatch):
    """The database replaced by one synthetic surface; counts calibrations."""
    stochastic_vol._cache.clear()
    calls = {"market": 0, "stochastic": 0}

    def local_vol_market(ticker, rate, valuation_date):
        calls["market"] += 1
        return _market()

    real = stochastic_vol._calibrate

    def calibrate(market):
        calls["stochastic"] += 1
        return real(market)

    monkeypatch.setattr(market_snapshot, "local_vol_market", local_vol_market)
    monkeypatch.setattr(stochastic_vol, "_calibrate", calibrate)
    monkeypatch.setattr(stochastic_vol, "SLV_PARTICLES", 20_000)
    yield calls
    stochastic_vol._cache.clear()


def _req(script, **kw):
    base = dict(script=script, rate=0.03, valuation_date=SNAP, n_paths=4000, seed=3)
    return ScriptRequest(**{**base, **kw})


def test_auto_is_the_default():
    assert _req(VANILLA, spot=100.0, vol=0.2).model == "auto"


def test_auto_needs_a_ticker_or_flat_vol_inputs():
    with pytest.raises(ValidationError, match="ticker"):
        _req(VANILLA)


def test_auto_without_a_ticker_is_flat_black_scholes_and_says_why():
    resp = pricing_service.price_script(_req(VANILLA, spot=100.0, vol=0.2))
    c = resp.model_choice
    assert (c.requested, c.model, c.code) == ("auto", "black_scholes", "no_market_data")
    assert c.calibration is None
    assert "flat_vol_smile" in {w.code for w in resp.warnings}


def test_a_vanilla_gets_local_vol_and_no_stochastic_calibration(store):
    resp = pricing_service.price_script(_req(VANILLA, ticker="TEST"))
    c = resp.model_choice
    assert (c.model, c.code) == ("local_vol", "terminal_smile")
    assert c.calibration.ticker == "TEST" and c.calibration.heston is None
    assert store["stochastic"] == 0


def test_a_barrier_gets_slv_with_its_calibration_announced(store):
    resp = pricing_service.price_script(_req(BARRIER, ticker="TEST"))
    c = resp.model_choice
    assert (c.model, c.code) == ("slv", "path_dependent")
    h, lev = c.calibration.heston, c.calibration.leverage
    assert h is not None and lev is not None
    assert -1.0 < h.rho < 0.0  # an equity put skew means negative spot/vol correlation
    assert h.iv_rmse < 0.02 and h.n_maturities == 5  # the 0.02y slice is below MIN_TTM
    assert 0.0 < lev.min <= lev.max and 0.0 <= lev.clamped_share < 1.0
    assert "model slv" in resp.diagnostics
    assert math.isfinite(resp.npv) and resp.npv > 0.0


def test_the_calibration_runs_once_per_snapshot(store):
    pricing_service.price_script(_req(BARRIER, ticker="TEST"))
    pricing_service.price_script(_req(BARRIER, ticker="TEST", seed=4))
    assert store["market"] == 2 and store["stochastic"] == 1


def test_a_hand_picked_model_is_honoured_and_labelled(store):
    resp = pricing_service.price_script(_req(VANILLA, ticker="TEST", model="heston"))
    c = resp.model_choice
    assert (c.requested, c.model, c.code) == ("heston", "heston", "user")
    assert c.calibration.heston is not None and c.calibration.leverage is None
    assert "heston_fit" in {w.code for w in resp.warnings}


def test_auto_falls_back_to_local_vol_when_the_calibration_fails(store, monkeypatch):
    def broken(market):
        raise stochastic_vol.CalibrationUnavailable("not enough maturities")

    monkeypatch.setattr(stochastic_vol, "_calibrate", broken)
    resp = pricing_service.price_script(_req(BARRIER, ticker="TEST"))
    assert (resp.model_choice.model, resp.model_choice.code) == (
        "local_vol",
        "stochastic_unavailable",
    )
    assert "stochastic_calibration_failed" in {w.code for w in resp.warnings}


def test_a_hand_picked_slv_that_cannot_calibrate_is_an_input_error(store, monkeypatch):
    def broken(market):
        raise stochastic_vol.CalibrationUnavailable("not enough maturities")

    monkeypatch.setattr(stochastic_vol, "_calibrate", broken)
    with pytest.raises(ValueError, match="cannot be calibrated"):
        pricing_service.price_script(_req(BARRIER, ticker="TEST", model="slv"))


def test_the_valuation_record_names_the_model_actually_used(store):
    req = _req(BARRIER, ticker="TEST")
    priced = valuation.price("script", req)
    spec = valuation.payload("script", req, priced, ip_hash=None).model
    assert spec.name == "slv"
    assert spec.params["requested"] == "auto"
    assert spec.calibration_id == f"TEST:{SNAP.isoformat()}"
    assert set(stochastic_vol.HESTON_KEYS) <= set(spec.params)


def test_heston_targets_read_the_svi_surface():
    # A flat SVI (b = 0) has one implied vol everywhere: every target is it.
    m = _market()
    m.svi_slices = [
        dict(ttm=t, a=0.04 * t, b=0.0, rho=0.0, m=0.0, sigma=0.1)
        for t in (0.02, 0.5, 1.0)
    ]
    strikes, ttms, vols = stochastic_vol.heston_targets(m)
    assert set(ttms) == {0.5, 1.0}  # 0.02 y is under MIN_TTM
    assert len(strikes) == 2 * len(stochastic_vol.Z_POINTS)
    assert all(abs(v - 0.2) < 1e-12 for v in vols)


def test_validate_announces_the_recommendation():
    from app.schemas import ScriptValidateRequest

    out = pricing_service.validate_script(
        ScriptValidateRequest(script=BARRIER, valuation_date=SNAP)
    )
    assert out.recommendation.model == "slv"
    assert out.recommendation.code == "path_dependent"
