"""The surface's reading of the quotes cleaned at ingestion (options.chain_iv,
WP 31 L5): which flags exclude a quote, the two-zero-bid stop, the European
proxy carried onto the product's forwards (D2), and the QM_OPTION_DATA
switch. The database and the calibrator are fakes; the native quote is the
real one when the module is built, so the scaling is checked on it."""

import math
import types
from datetime import date

import pytest

from app import chain_iv, db, market_snapshot as ms
from app.chain_iv import Flag

SNAP = date(2026, 11, 26)
EXPIRY = date(2027, 11, 26)


def row(
    strike,
    kind,
    *,
    flags=0,
    forward=100.0,
    discount=0.96,
    iv=0.2,
    ttm=1.0,
    bid=1.0,
    ask=1.1,
    root="X",
):
    return db.ChainIvRow(
        expiry=EXPIRY,
        root=root,
        option_type=kind,
        strike=strike,
        ttm=ttm,
        bid=bid,
        ask=ask,
        mid=0.5 * (bid + ask),
        exercise="european",
        forward=forward,
        discount=discount,
        implied_vol=iv,
        log_moneyness=math.log(strike / forward),
        flags=flags,
    )


# ── usable(): the flags of WP 31 §6.1 ──────────────────────────────────────


@pytest.mark.parametrize(
    "flag",
    [
        Flag.NO_BID,
        Flag.WIDE_SPREAD,
        Flag.BELOW_TWO_TICKS,
        Flag.PARITY_OUTLIER,
        Flag.EXPIRY_REJECTED,
        Flag.IV_FAILED,
        Flag.NOT_OTM,
        Flag.SHORT_MATURITY,
    ],
)
def test_an_excluding_flag_drops_the_quote(flag):
    assert chain_iv.usable([row(110.0, "call", flags=flag)]) == []


@pytest.mark.parametrize("flag", [Flag.STALE, Flag.RATE_FALLBACK, 0])
def test_stale_and_rate_fallback_do_not_drop_it(flag):
    # STALE is wrong on a holiday's snapshot (data-ingest#34); the fallback
    # rate is a warning, not a reason to drop a quote.
    assert len(chain_iv.usable([row(110.0, "call", flags=flag)])) == 1


def test_two_consecutive_zero_bids_end_the_side_away_from_the_forward():
    puts = [
        row(95.0, "put"),
        row(90.0, "put", flags=Flag.NO_BID),
        row(85.0, "put"),  # one zero bid alone does not stop
        row(80.0, "put", flags=Flag.NO_BID),
        row(75.0, "put", flags=Flag.NO_BID),  # the second in a row: stop
        row(70.0, "put"),  # beyond the stop, despite its bid
    ]
    calls = [
        row(105.0, "call"),
        row(110.0, "call", flags=Flag.NO_BID),
        row(115.0, "call"),
    ]
    kept = sorted(r.strike for r in chain_iv.usable(puts + calls))
    assert kept == [85.0, 95.0, 105.0, 115.0]


def test_the_zero_bid_stop_is_per_side_and_per_root():
    # A stop on one root's puts does not reach the other root's.
    rows = [
        row(90.0, "put", root="SPX", flags=Flag.NO_BID),
        row(85.0, "put", root="SPX", flags=Flag.NO_BID),
        row(80.0, "put", root="SPX"),
        row(80.0, "put", root="SPXW"),
    ]
    assert [(r.root, r.strike) for r in chain_iv.usable(rows)] == [("SPXW", 80.0)]


def test_an_itm_side_does_not_count_towards_the_stop():
    rows = [
        row(99.0, "call", flags=Flag.NOT_OTM | Flag.NO_BID),
        row(101.0, "call", flags=Flag.NO_BID),
        row(105.0, "call"),
    ]
    assert [r.strike for r in chain_iv.usable(rows)] == [105.0]


# ── D2: the European proxy carried onto the product's forwards ─────────────


def expiry(ttm, forward, spot=50.0):
    return db.ChainIvExpiry(EXPIRY, "Y", ttm, forward, 0.96, spot, "parity")


def test_the_carry_curve_reprices_its_own_expiries_and_is_flat_beyond():
    c = chain_iv.CarryCurve.of([expiry(0.5, 50.5), expiry(1.0, 51.5)], 50.0)
    assert c.forward(0.5) == pytest.approx(50.5, rel=1e-14)
    assert c.forward(1.0) == pytest.approx(51.5, rel=1e-14)
    mid = 0.5 * (math.log(50.5 / 50) / 0.5 + math.log(51.5 / 50))
    assert c.carry(0.75) == pytest.approx(mid, rel=1e-14)
    assert c.carry(0.1) == c.carry(0.5) and c.carry(3.0) == c.carry(1.0)


def test_no_accepted_expiry_is_an_error():
    with pytest.raises(ValueError):
        chain_iv.CarryCurve.of([], 50.0)


def test_a_carried_quote_keeps_its_vol_and_moneyness_at_the_target_scale():
    r = row(4200.0, "call", forward=4000.0, discount=0.95, iv=0.18, bid=10.0, ask=11.0)
    target = chain_iv.CarryCurve.of([expiry(1.0, 400.0 * 1.01, spot=400.0)], 400.0)
    (q,) = chain_iv.carried_onto([r], target)
    f = target.forward(1.0)
    assert q.forward == pytest.approx(f, rel=1e-14)
    assert math.log(q.strike / q.forward) == pytest.approx(r.log_moneyness, abs=1e-14)
    assert q.implied_vol == r.implied_vol and q.discount == r.discount and q.has_iv
    s = f / 4000.0
    assert (q.bid, q.ask) == pytest.approx((10.0 * s, 11.0 * s), rel=1e-14)
    # The relative spread, the fit's weight, is unchanged.
    assert (q.ask - q.bid) / q.ask == pytest.approx(1.0 / 11.0, rel=1e-12)


# ── market_snapshot under QM_OPTION_DATA ───────────────────────────────────


def test_the_switch_defaults_to_legacy_and_refuses_an_unknown_value(monkeypatch):
    monkeypatch.delenv(ms.OPTION_DATA_ENV, raising=False)
    assert ms.option_data() == "legacy"
    monkeypatch.setenv(ms.OPTION_DATA_ENV, " Chain_IV ")
    assert ms.option_data() == "chain_iv"
    monkeypatch.setenv(ms.OPTION_DATA_ENV, "yahoo")
    with pytest.raises(ValueError):
        ms.option_data()


@pytest.fixture
def store(monkeypatch):
    """A computed SPY chain, and optionally a ^SPX one on the same day."""
    ms.clear_caches()
    monkeypatch.setenv(ms.OPTION_DATA_ENV, "chain_iv")
    state = dict(
        loads={
            "SPY": db.ChainIvLoad(SNAP, 2, "spy-sum", 1, "parity"),
            "^SPX": db.ChainIvLoad(SNAP, 2, "spx-sum", 1, "parity"),
        },
        rows={
            "SPY": [row(55.0, "call", forward=50.5), row(45.0, "put", forward=50.5)],
            "^SPX": [
                row(5500.0, "call", forward=5050.0),
                row(4500.0, "put", forward=5050.0),
                row(4000.0, "put", forward=5050.0, flags=Flag.PARITY_OUTLIER),
            ],
        },
        calls=[],
    )
    monkeypatch.setattr(
        db, "options_chain_iv_load_on_or_before", lambda t, d: state["loads"].get(t)
    )
    monkeypatch.setattr(db, "options_chain_iv", lambda t, d: state["rows"][t])
    monkeypatch.setattr(
        db, "options_chain_iv_expiries", lambda t, d: [expiry(1.0, 50.5)]
    )
    monkeypatch.setattr(db, "price_on_or_before", lambda t, d: (SNAP, 50.0))
    monkeypatch.setattr(db, "dividend_yield_on_or_before", lambda t, d: (SNAP, 0.01))

    def calibrate(quotes, spot, rate, dividend, *a, cleaning_params, **k):
        state["calls"].append((quotes, cleaning_params))
        state.setdefault("kwargs", []).append(k)
        return {
            "K_grid": [40.0, 60.0],
            "T_grid": [0.1, 1.0],
            "sigma_loc_flat": [0.2] * 4,
        }

    monkeypatch.setattr(
        ms,
        "qm",
        types.SimpleNamespace(
            CleaningParams=types.SimpleNamespace, calibrate_vol_surface=calibrate
        ),
    )
    yield state
    ms.clear_caches()


def codes(m):
    return [w["code"] for w in m.warnings]


def test_spy_is_priced_on_the_spx_surface_of_the_same_day(store):
    m = ms.local_vol_market("SPY", 0.04, SNAP)
    quotes, cleaning = store["calls"][0]
    assert cleaning.ingestion_cleaned is True
    assert m.option_data == "chain_iv"
    # SPX's two usable quotes, at SPY's scale (the parity outlier is out).
    assert sorted(round(q.strike, 9) for q in quotes) == [45.0, 55.0]
    assert all(q.forward == pytest.approx(50.5) for q in quotes)
    assert "european_proxy_surface" in codes(m)
    assert "market_carry" in codes(m)


def test_without_an_spx_chain_that_day_spy_takes_its_own_and_says_so(store):
    store["loads"]["^SPX"] = db.ChainIvLoad(date(2026, 11, 20), 2, "old", 1, "parity")
    m = ms.local_vol_market("SPY", 0.04, SNAP)
    quotes, _ = store["calls"][0]
    assert sorted(q.strike for q in quotes) == [45.0, 55.0]
    assert "own_american_surface" in codes(m)
    assert "european_proxy_surface" not in codes(m)


def test_a_fred_discount_is_reported(store):
    store["loads"]["SPY"] = db.ChainIvLoad(SNAP, 2, "spy-sum", 1, "fred")
    assert "rate_fallback" in codes(ms.local_vol_market("SPY", 0.04, SNAP))


def test_a_recomputed_proxy_is_a_new_surface(store):
    ms.local_vol_market("SPY", 0.04, SNAP)
    ms.local_vol_market("SPY", 0.04, SNAP)
    assert len(store["calls"]) == 1
    store["loads"]["^SPX"] = db.ChainIvLoad(SNAP, 2, "spx-recomputed", 1, "parity")
    ms.local_vol_market("SPY", 0.04, SNAP)
    assert len(store["calls"]) == 2


def test_no_computed_chain_names_the_source_to_run(store):
    store["loads"].pop("SPY")
    with pytest.raises(ms.MarketDataUnavailable, match="options-chain-iv"):
        ms.local_vol_market("SPY", 0.04, SNAP)


# ── QM_VOL_SURFACE (WP 30 E2) ──────────────────────────────────────────────


def test_the_surface_switch_defaults_to_svi_and_refuses_an_unknown_value(monkeypatch):
    monkeypatch.delenv(ms.VOL_SURFACE_ENV, raising=False)
    assert ms.vol_surface() == "svi"
    monkeypatch.setenv(ms.VOL_SURFACE_ENV, " eSSVI ")
    assert ms.vol_surface() == "essvi"
    monkeypatch.setenv(ms.VOL_SURFACE_ENV, "sabr")
    with pytest.raises(ValueError):
        ms.vol_surface()


def test_essvi_is_asked_of_the_calibration_and_is_its_own_surface(store, monkeypatch):
    monkeypatch.delenv(ms.VOL_SURFACE_ENV, raising=False)
    ms.local_vol_market("SPY", 0.04, SNAP)
    assert "surface_model" not in store["kwargs"][-1]  # the SVI call unchanged
    monkeypatch.setenv(ms.VOL_SURFACE_ENV, "essvi")
    ms.local_vol_market("SPY", 0.04, SNAP)
    assert store["kwargs"][-1]["surface_model"] == "essvi"
    assert len(store["calls"]) == 2  # a new surface, not the cached SVI one
