"""Calibrations made once (issue #90): the surface is cached on a digest of
what was read, the stochastic-vol fit is stored and survives a restart, and
the warm-up calibrates each tracked ticker's new snapshot at the rates it was
asked for."""

from datetime import date

import pytest

from app import calibration_warmup, db, market_snapshot, stochastic_vol

SNAP = date(2026, 9, 11)


def _svi(T: float) -> dict:
    return dict(ttm=T, a=0.03 * T, b=0.1 * T, rho=-0.6, m=0.0, sigma=0.1)


def _market(ticker="TEST", rate=0.03, level=0.2) -> market_snapshot.LocalVolMarket:
    K = [50.0 + 5.0 * i for i in range(21)]
    T = [0.1, 0.5, 1.0, 2.0]
    sigma = [level + 0.3 * max(0.0, (100.0 - k) / 100.0) for k in K for _ in T]
    return market_snapshot.LocalVolMarket(
        ticker=ticker,
        valuation_date=SNAP,
        spot=100.0,
        dividend=0.0,
        K_grid=K,
        T_grid=T,
        sigma_loc_flat=sigma,
        svi_slices=[_svi(t) for t in (0.1, 0.25, 0.5, 1.0, 2.0)],
        rate=rate,
    )


@pytest.fixture
def fits(monkeypatch):
    """Counts the real calibrations (fewer particles: a test, not a price)."""
    calls = []
    real = stochastic_vol._calibrate

    def calibrate(market):
        calls.append((market.ticker, market.rate))
        return real(market)

    monkeypatch.setattr(stochastic_vol, "_calibrate", calibrate)
    monkeypatch.setattr(stochastic_vol, "SLV_PARTICLES", 5_000)
    return calls


def test_a_restart_reads_the_stored_fit(fits):
    first = stochastic_vol.calibrate(_market())
    stochastic_vol._cache.clear()  # a new process: only the store is left
    again = stochastic_vol.calibrate(_market())
    assert fits == [("TEST", 0.03)]
    assert again == first


def test_a_changed_surface_or_method_is_calibrated_again(fits, monkeypatch):
    stochastic_vol.calibrate(_market())
    stochastic_vol.calibrate(_market(level=0.25))  # a revised surface
    assert len(fits) == 2
    stochastic_vol._cache.clear()
    monkeypatch.setattr(stochastic_vol, "METHOD", stochastic_vol.METHOD + 1)
    stochastic_vol.calibrate(_market())
    assert len(fits) == 3


def test_the_last_rates_are_remembered_most_recent_first(fits):
    for rate in (0.03, 0.04, 0.03, 0.05, 0.06):
        stochastic_vol.calibrate(_market(rate=rate))
    assert stochastic_vol.remembered_rates("TEST") == [0.06, 0.05, 0.03]
    assert stochastic_vol.remembered_rates("OTHER") == []


def test_the_warm_up_calibrates_the_new_snapshot_at_the_remembered_rates(
    fits, monkeypatch
):
    stochastic_vol.calibrate(_market(rate=0.04))
    fits.clear()
    stochastic_vol._cache.clear()
    # A new snapshot lands: a different surface for the same ticker.
    monkeypatch.setattr(db, "options_chain_tickers", lambda since: ["TEST", "NEW"])

    def local_vol_market(ticker, rate, today):
        if ticker == "NEW":
            raise market_snapshot.MarketDataUnavailable("no close")
        return _market(ticker, rate, level=0.22)

    monkeypatch.setattr(market_snapshot, "local_vol_market", local_vol_market)
    done = calibration_warmup.warm_once(SNAP)
    assert done == [("TEST", 0.04, f"ready for {SNAP.isoformat()}")]
    assert fits == [("TEST", 0.04)]
    # The request that follows finds it ready.
    stochastic_vol.calibrate(_market(rate=0.04, level=0.22))
    assert fits == [("TEST", 0.04)]


def test_the_warm_up_reports_a_ticker_it_cannot_calibrate(fits, monkeypatch):
    stochastic_vol.calibrate(_market(rate=0.03))
    monkeypatch.setattr(db, "options_chain_tickers", lambda since: ["TEST"])

    def unavailable(ticker, rate, today):
        raise market_snapshot.MarketDataUnavailable("the snapshot is too old")

    monkeypatch.setattr(market_snapshot, "local_vol_market", unavailable)
    [(ticker, rate, outcome)] = calibration_warmup.warm_once(SNAP)
    assert (ticker, rate) == ("TEST", 0.03) and outcome.startswith("skipped")


def test_the_warm_up_is_off_unless_asked(monkeypatch):
    monkeypatch.delenv("QM_CALIBRATION_WARMUP", raising=False)
    assert not calibration_warmup.enabled()
    monkeypatch.setenv("QM_CALIBRATION_WARMUP", "1")
    assert calibration_warmup.enabled()
