"""Market data comes from the database and nowhere else. These tests pin the
whole API to that rule: no yfinance import, a read-only db module, and a
missing value reported as an error rather than fetched from anywhere."""

import ast
import subprocess
import sys
from pathlib import Path

import pytest
from fastapi import HTTPException

from app import db, vol_surface
from app.routers import local_vol_pricing

API_APP = Path(__file__).resolve().parents[1] / "app"


def test_nothing_in_the_api_imports_yfinance():
    offenders = []
    for path in API_APP.rglob("*.py"):
        for node in ast.walk(ast.parse(path.read_text())):
            names = []
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom):
                names = [node.module or ""]
            if any(n.split(".")[0] == "yfinance" for n in names):
                offenders.append(f"{path.relative_to(API_APP)}:{node.lineno}")
    assert offenders == []


def test_loading_every_market_data_module_does_not_load_yfinance():
    code = (
        "import sys; sys.path.insert(0, 'api');"
        "import app.db, app.vol_surface, app.market_snapshot;"
        "import app.routers.local_vol_pricing, app.routers.simulation;"
        "print('yfinance' in sys.modules)"
    )
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert out.stdout.strip() == "False", out.stderr


def test_the_db_module_never_writes():
    """data-ingest is the only writer: a value the API cannot find must be
    reported missing, never fetched elsewhere and written back."""
    src = (API_APP / "db.py").read_text().upper()
    for verb in ("INSERT ", "UPDATE ", "DELETE ", "CREATE TABLE", "DROP "):
        assert verb not in src, verb
    assert not hasattr(db, "cache_option_chain_snapshot")


@pytest.fixture
def empty_store(monkeypatch):
    monkeypatch.setattr(db, "options_chain_snapshot", lambda t, as_of=None: [])
    monkeypatch.setattr(db, "latest_price", lambda t: None)
    monkeypatch.setattr(db, "latest_dividend_yield", lambda t: None)


def test_a_ticker_absent_from_the_database_is_an_error_not_a_live_fetch(empty_store):
    with pytest.raises(vol_surface.NoOptionChainAvailable, match="OPTIONS_CHAIN_TICKERS"):
        vol_surface.fetch_option_chain("ZZZZ")
    with pytest.raises(vol_surface.NoOptionChainAvailable, match="sp500-prices"):
        vol_surface.get_spot("ZZZZ")
    with pytest.raises(vol_surface.NoOptionChainAvailable, match="dividend"):
        vol_surface.get_dividend_yield("ZZZZ")


def test_an_unreachable_database_propagates_instead_of_falling_back(monkeypatch):
    def down(*a, **k):
        raise db.StoreUnavailable("connection refused")

    monkeypatch.setattr(db, "options_chain_snapshot", down)
    with pytest.raises(db.StoreUnavailable):
        vol_surface.fetch_option_chain("SPY")


def test_the_local_vol_endpoints_answer_404_for_missing_data(empty_store):
    with pytest.raises(HTTPException) as e:
        local_vol_pricing._calibrate("ZZZZ", 0.03, 10, 0.05, 0.5, 0.7, 1.4)
    assert e.value.status_code == 404


def test_the_local_vol_endpoints_answer_503_when_the_database_is_down(monkeypatch):
    def down(*a, **k):
        raise db.StoreUnavailable("connection refused")

    monkeypatch.setattr(db, "latest_price", down)
    with pytest.raises(HTTPException) as e:
        local_vol_pricing._calibrate("SPY", 0.03, 10, 0.05, 0.5, 0.7, 1.4)
    assert e.value.status_code == 503
