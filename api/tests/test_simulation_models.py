"""The simulation page's new dynamics: local vol, Heston and SLV paths, and
their calibration on a stored surface (never a live source). The native
module simulates and calibrates for real; the database is a synthetic
surface."""

import math

import pytest
from fastapi import HTTPException
from pydantic import ValidationError

from app import market_snapshot, stochastic_vol
from app.routers import simulation
from app.schemas import (
    ModelPathRequest,
    SimulationCalibrateRequest,
    SimulationModel,
)

from test_script_model_choice import _market  # noqa: E402 (tests dir on sys.path)


@pytest.fixture
def store(monkeypatch):
    stochastic_vol._cache.clear()
    monkeypatch.setattr(market_snapshot, "local_vol_market", lambda *a: _market())
    monkeypatch.setattr(stochastic_vol, "SLV_PARTICLES", 20_000)
    yield
    stochastic_vol._cache.clear()


@pytest.mark.parametrize("model", ["local_vol", "heston", "slv"])
def test_every_surface_model_simulates_from_a_ticker(store, model):
    out = simulation.simulate_model(
        ModelPathRequest(model=model, ticker="TEST", ttm=1.0, n_steps=12, n_paths=200)
    )
    assert out.model == SimulationModel(model)
    assert len(out.time_grid) == 13 and len(out.paths) == 200
    assert all(p[0] == 100.0 and all(x > 0 for x in p) for p in out.paths)
    # the risk-neutral mean stays near the forward (200 paths: loose)
    mean = sum(p[-1] for p in out.paths) / 200
    assert mean == pytest.approx(100 * math.exp(0.05), rel=0.1)


def test_heston_runs_on_typed_parameters_without_a_ticker():
    out = simulation.simulate_model(
        ModelPathRequest(
            model="heston",
            spot=50.0,
            heston=dict(v0=0.04, kappa=2.0, theta=0.04, xi=0.5, rho=-0.7),
            ttm=0.5,
            n_steps=10,
            n_paths=5,
        )
    )
    assert out.paths[0][0] == 50.0


def test_local_vol_and_slv_refuse_to_run_without_a_ticker():
    for model in ("local_vol", "slv"):
        with pytest.raises(ValidationError, match="ticker"):
            ModelPathRequest(model=model, spot=100.0, ttm=1.0)


def test_calibration_reports_the_heston_fit_and_the_leverage(store):
    out = simulation.calibrate(
        SimulationCalibrateRequest(ticker="TEST", model="slv", ttm=1.0, rate=0.03)
    )
    assert out.heston.iv_rmse < 0.02 and -1 < out.heston.rho < 0
    assert out.leverage.n_particles == 20_000
    assert out.snapshot == _market().valuation_date
    assert "SVI slices" in out.surface and out.slice_ttm is None


def test_a_missing_chain_is_a_422_naming_what_is_missing(monkeypatch):
    def missing(*a):
        raise market_snapshot.MarketDataUnavailable(
            "no option-chain snapshot for 'ZZZ'"
        )

    monkeypatch.setattr(market_snapshot, "local_vol_market", missing)
    with pytest.raises(HTTPException) as e:
        simulation.calibrate(
            SimulationCalibrateRequest(ticker="ZZZ", model="heston", ttm=1.0)
        )
    assert e.value.status_code == 422 and "ZZZ" in e.value.detail
