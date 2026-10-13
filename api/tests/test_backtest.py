"""The backtest endpoints (blueprint/wp/24-backtest.md, lot B8): the real
`quantbacktest` wheel on a market written here, the database replaced by
plain lists.

What is checked is this repository's half: what is read and how it is shaped,
that nothing is filled or shared between windows, that every figure comes out
with its interval and convention, and that a recorded run replays.
"""

from __future__ import annotations

from datetime import date, timedelta

import numpy as np
import pytest
from fastapi.testclient import TestClient

from api.app import db
from api.app.audit.payloads import BacktestRunPayload
from api.app.audit.sinks import (
    InMemorySink,
    reset_sink_for_testing,
    set_sink_for_testing,
)
from api.app.backtest import engine, market, record, universe
from api.app.backtest import schemas as s
from api.app.main import app
from api.app.replay import NotAValuation, ReplayOutcome

pytest.importorskip("quantbacktest", reason="run scripts/install_quantbacktest.sh")

TICKERS = ("AAA", "BBB", "CCC", "DDD", "SPY")


class Store:
    """A market of five tickers on business days, as the tables hold it."""

    def __init__(self, seed: int = 0):
        rng = np.random.default_rng(seed)
        days = np.arange("2015-01-01", "2021-01-01", dtype="datetime64[D]")
        self.days = [d.astype(object) for d in days[np.is_busday(days)]]
        returns = rng.normal(0.0004, 0.012, (len(self.days), len(TICKERS)))
        self.close = 100.0 * np.cumprod(1.0 + returns, axis=0)
        self.calls: list[tuple] = []
        #: (ticker, day) without a quote.
        self.holes: set[tuple[str, date]] = set()

    def bars(self, tickers, start, end):
        self.calls.append((tuple(tickers), start, end))
        rows = []
        for i, day in enumerate(self.days):
            if not start <= day <= end:
                continue
            for t in sorted(tickers):
                if t not in TICKERS or (t, day) in self.holes:
                    continue
                c = float(self.close[i, TICKERS.index(t)])
                rows.append((day, t, c * 0.999, c * 1.004, c * 0.996, c, 1_000_000))
        return rows

    def dividends(self, tickers, start, end):
        return [
            (t, day, 0.5)
            for day in self.days[::63]
            if start <= day <= end
            for t in sorted(tickers)
            if t in ("AAA", "SPY")
        ]

    def fred(self, series_id, start, end):
        # No value on Fridays' following Mondays would be too clever: skip
        # every tenth day, as a bond-market holiday does.
        return [
            (day, 1.5)
            for i, day in enumerate(self._calendar(start, end))
            if day.weekday() < 5 and i % 10
        ]

    def french(self, dataset, start, end):
        rng = np.random.default_rng(3)
        names = (*market.FACTOR_DATASETS[dataset], "RF")
        return [
            (day, name, float(rng.normal(0.02, 0.8)) if name != "RF" else 0.006)
            for day in self.days
            if start <= day <= min(end, date(2020, 10, 30))
            for name in names
        ]

    @staticmethod
    def _calendar(start, end):
        return [start + timedelta(days=i) for i in range((end - start).days + 1)]


@pytest.fixture
def store(monkeypatch):
    fake = Store()
    monkeypatch.setattr(db, "raw_daily_bars", fake.bars)
    monkeypatch.setattr(db, "dividends", fake.dividends)
    monkeypatch.setattr(db, "splits", lambda tickers, start, end: [])
    monkeypatch.setattr(db, "fred_first_vintage", fake.fred)
    monkeypatch.setattr(db, "french_first_vintage", fake.french)
    market._markets._store.clear()
    yield fake
    market._markets._store.clear()


@pytest.fixture
def sink():
    memory = InMemorySink()
    set_sink_for_testing(memory)
    yield memory
    reset_sink_for_testing()


@pytest.fixture
def client():
    return TestClient(app)


STRATEGY = dict(
    tickers=["AAA", "BBB", "CCC", "DDD"],
    start="2016-06-01",
    end="2020-12-31",
    initial_capital=1_000_000,
    proportional_cost_bps=5,
    method="minimum_variance",
    lookback=120,
    rebalance="monthly",
    max_weight=0.5,
    bootstrap_replications=200,
    mean_block_length=5,
    seed=11,
    newey_west_lags=6,
)

VALIDATION = dict(
    tickers=STRATEGY["tickers"],
    start="2016-06-01",
    end="2020-12-31",
    initial_capital=1_000_000,
    proportional_cost_bps=5,
    grid=[
        dict(method="minimum_variance", lookback=60, rebalance="monthly"),
        dict(method="minimum_variance", lookback=120, rebalance="monthly"),
        dict(method="risk_parity", lookback=120, rebalance=21),
        dict(method="equal_weight", lookback=60, rebalance="monthly"),
    ],
    train=252,
    test=63,
    groups=6,
    test_groups=2,
    purge=5,
    embargo=5,
    blocks=8,
)


def run(req=STRATEGY) -> s.StrategyResponse:
    return engine.run_strategy(s.StrategyRequest(**req))


# ── What is read ─────────────────────────────────────────────────────────────


def test_two_windows_never_share_their_prices(store):
    """Acceptance criterion of WP 24 (finding 1 of the old page): the cache
    key holds the dates."""
    first = run()
    second = run({**STRATEGY, "start": "2018-01-02"})
    windows = {
        call[1:] for call in store.calls if call[0] == tuple(STRATEGY["tickers"])
    }
    assert len(windows) == 2
    assert first.dates[0] == date(2016, 6, 1)
    assert second.dates[0] == date(2018, 1, 2)
    assert first.data.fingerprints["market"] != second.data.fingerprints["market"]
    # The same window again is served from the cache, with the same data.
    reads = len(store.calls)
    assert run().data.fingerprints == first.data.fingerprints
    assert len(store.calls) == reads


def test_a_missing_quote_stays_missing(store):
    hole = store.days[400]
    store.holes.add(("BBB", hole))
    table = market.market(("AAA", "BBB"), store.days[300], store.days[500])
    row = list(table.dates.astype(object)).index(hole)
    assert np.isnan(table.close[row, 1]) and np.isnan(table.volume[row, 1])
    assert np.isfinite(table.close[row, 0])
    assert np.isnan(table.close).sum() == 1
    assert (np.diff(table.dates) > np.timedelta64(0, "D")).all()


def test_the_book_opens_on_the_start_date_already_invested(store):
    """The estimation window is read from before `start`: the first decision
    is taken on the first day, not a look-back later."""
    result = run()
    assert result.allocations.dates[0] == date(2016, 6, 1)
    assert result.strategy.value[0] == 1_000_000
    assert len(result.strategy.value) == len(result.dates)
    assert len(result.benchmark.returns) == len(result.dates) - 1


def test_the_cash_rate_is_the_last_one_published(store):
    """A day FRED has no value for carries the previous one: an as-of read,
    in decimal."""
    days = np.array(store.days[100:140], dtype="datetime64[D]")
    assert np.allclose(market.annual_cash_rate(days), 0.015)


def test_factors_are_handed_over_as_decimals_one_dataset_at_a_time(store):
    f = market.factors("ff5", date(2016, 1, 1), date(2016, 12, 31))
    assert f.names == market.FACTOR_DATASETS["ff5"]
    assert f.values.shape == (len(f.dates), 5)
    assert np.allclose(f.risk_free, 0.00006)
    assert np.abs(f.values).max() < 0.1


# ── Missing data is an error ────────────────────────────────────────────────


def test_a_ticker_without_quotes_is_named(store, client, sink):
    response = client.post(
        "/api/backtest/strategy", json={**STRATEGY, "tickers": ["AAA", "ZZZ"]}
    )
    assert response.status_code == 422
    assert "ZZZ" in response.json()["message"]
    assert not [e for e in sink.events if e.type == "backtest.run"]
    assert not [e for e in sink.events if e.type == "data.fallback"]


def test_no_cash_rate_is_an_error_not_a_default(store, monkeypatch):
    monkeypatch.setattr(db, "fred_first_vintage", lambda *a: [])
    with pytest.raises(market.MissingData, match="DGS3MO"):
        run()


def test_the_engine_s_refusal_is_shown_as_it_is(store, client):
    response = client.post(
        "/api/backtest/strategy", json={**STRATEGY, "max_weight": 0.1}
    )
    assert response.status_code == 422
    assert "infeasible" in response.json()["message"]


# ── What comes out ───────────────────────────────────────────────────────────


def _metrics(node):
    """Every Metric of a response, wherever it sits."""
    if isinstance(node, s.Metric):
        yield node
    elif isinstance(node, s.BaseModel):
        for name in type(node).model_fields:
            yield from _metrics(getattr(node, name))
    elif isinstance(node, dict):
        for value in node.values():
            yield from _metrics(value)
    elif isinstance(node, (list, tuple)):
        for value in node:
            yield from _metrics(value)


def test_no_figure_without_its_interval_and_its_convention(store):
    """Acceptance criterion of WP 24."""
    result = run()
    found = list(_metrics(result))
    assert len(found) > 40
    for metric in found:
        assert metric.method and metric.convention
        assert metric.lower <= metric.value <= metric.upper
        assert 0 < metric.confidence < 1
    for book in (
        result.strategy,
        result.gross,
        result.equal_weight,
        result.buy_and_hold,
    ):
        assert "sharpe_ratio" in book.report.metrics
        assert set(book.report.omitted).isdisjoint(book.report.metrics)


def test_net_and_gross_come_side_by_side(store):
    result = run()
    assert result.gross.value[-1] > result.strategy.value[-1]
    assert result.cost_summary.annualized_turnover > 0
    assert result.cost_summary.annual_cost_bps > 0
    assert "5 basis points" in result.conventions["costs"]
    free = run({**STRATEGY, "proportional_cost_bps": 0})
    assert free.gross is None


def test_the_quality_report_names_tickers(store):
    store.close[700:, 1] *= 3.0  # a jump no split explains
    result = run()
    flagged = [q for q in result.quality if q.ticker == "BBB"]
    assert flagged and flagged[0].message.startswith("BBB:")
    assert "asset 1" not in flagged[0].message


def test_the_regression_stops_where_the_factors_do(store):
    result = run()
    assert result.factor_regression.last == date(2020, 10, 30)
    assert result.factor_regression.factors == ["Mkt-RF", "SMB", "HML"]
    assert len(result.factor_regression.betas) == 3


def test_validation_counts_every_configuration_as_a_trial(store, client, sink):
    response = client.post("/api/backtest/validation", json=VALIDATION)
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["deflated_sharpe"]["trials"] == len(VALIDATION["grid"])
    assert len(body["descriptions"]) == len(VALIDATION["grid"])
    fold = body["walk_forward"]["folds"][0]
    assert (
        fold["train_begin"] < fold["train_end"] < fold["test_begin"] <= fold["test_end"]
    )
    assert body["overfitting"]["splits"] == len(body["overfitting"]["logits"])
    assert "standard_error" not in body["overfitting"]
    (event,) = [e for e in sink.events if e.type == "backtest.run"]
    assert event.payload["kind"] == "validation"


# ── Audit and replay ─────────────────────────────────────────────────────────


def _recorded(client, sink, req=STRATEGY) -> dict:
    response = client.post("/api/backtest/strategy", json=req)
    assert response.status_code == 200, response.text
    (event,) = [e for e in sink.events if e.type == "backtest.run"]
    return event.model_dump(mode="json")


def test_a_run_is_recorded_with_what_replays_it(store, client, sink):
    event = _recorded(client, sink)
    payload = event["payload"]
    assert payload["request"]["seed"] == 11
    assert list(payload["fingerprints"]) == [
        "market",
        "cash_rate",
        "benchmark",
        "factors",
    ]
    assert all(len(f) == 64 for f in payload["fingerprints"].values())
    assert len(payload["engine"]["git_sha"]) >= 40
    assert len(payload["engine"]["quantmodeling_sha"]) == 40
    assert payload["headline"]["sharpe_ratio"] is not None
    assert not {"password", "token", "authorization"} & set(
        BacktestRunPayload.model_fields
    )


def test_a_replayed_run_gives_the_same_figures(store, client, sink):
    """Acceptance criterion of WP 24: to the bit, whatever was cached."""
    event = _recorded(client, sink)
    market._markets._store.clear()
    result = record.replay(event)
    assert result.outcome == ReplayOutcome.REPRODUCED
    assert result.differences == [] and result.drifted_inputs == []
    assert result.replayed_headline == result.recorded_headline


def test_replay_names_the_table_that_was_revised(store, client, sink):
    event = _recorded(client, sink)
    store.close[900, 0] *= 1.01  # data-ingest rewrote one close
    market._markets._store.clear()
    result = record.replay(event)
    assert result.outcome == ReplayOutcome.DRIFTED_INPUTS
    assert [t.name for t in result.drifted_inputs] == ["market"]


def test_replay_names_another_engine_build(store, client, sink):
    event = _recorded(client, sink)
    event["payload"]["engine"]["git_sha"] = "0" * 40
    result = record.replay(event)
    assert result.outcome == ReplayOutcome.DRIFTED_CODE
    assert result.recorded_engine.git_sha != result.current_engine.git_sha


def test_replay_never_passes_a_different_figure_silently(store, client, sink):
    event = _recorded(client, sink)
    event["payload"]["headline"]["sharpe_ratio"] += 1e-9
    result = record.replay(event)
    assert result.outcome == ReplayOutcome.NOT_REPRODUCED
    assert [d.name for d in result.differences] == ["sharpe_ratio"]


def test_replay_refuses_a_request_the_api_no_longer_accepts(store, client, sink):
    event = _recorded(client, sink)
    event["payload"]["request"]["method"] = "momentum"
    with pytest.raises(NotAValuation):
        record.replay(event)


# ── Universe ─────────────────────────────────────────────────────────────────


def test_the_universe_lists_the_funds_first(monkeypatch, client):
    monkeypatch.setattr(
        db,
        "market_members",
        lambda _m: [db.MarketMember("AAPL", "Apple Inc.", "equity", "USD")],
    )
    monkeypatch.setattr(
        db,
        "raw_daily_coverage",
        lambda tickers: {
            t: (date(2005, 1, 3), date(2026, 10, 9), 5000)
            for t in tickers
            if t in ("SPY", "TLT", "AAPL")
        },
    )
    universe._cache._store.clear()
    body = client.get("/api/backtest/universe").json()
    universe._cache._store.clear()
    assert [a["ticker"] for a in body["etfs"]] == ["SPY", "TLT"]
    assert body["stocks"][0]["name"] == "Apple Inc."
    assert body["benchmark"] == "SPY" and body["cash_series"] == "DGS3MO"
    assert len(body["engine"]["git_sha"]) >= 40
