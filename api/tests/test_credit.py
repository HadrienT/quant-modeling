"""Credit page — `credit.py`, `fundamentals.py` and `/credit/*`.

The real C++ library (the wheel) on a fake store: the hazard curve reprices
the spreads it was built from, a spread curve that falls too steeply is
reported rather than drawn, statements pick concepts in their stated order and
show the latest restatement, derived lines follow their formulas, and the
structural model refuses the companies it does not apply to.
"""

from __future__ import annotations

import math
import os
from datetime import date, timedelta

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

import quantmodeling as qm
from api.app import credit, db, fundamentals
from api.app.main import app
from api.app.routers import credit as credit_router

TODAY = date.today()
AS_OF = TODAY - timedelta(days=1)

USD_PCT = {
    "DGS1MO": 3.97,
    "DGS3MO": 4.14,
    "DGS6MO": 4.24,
    "DGS1": 4.44,
    "DGS2": 4.76,
    "DGS3": 4.83,
    "DGS5": 4.86,
    "DGS7": 4.93,
    "DGS10": 5.01,
    "DGS20": 5.38,
    "DGS30": 5.34,
}
# ICE BofA OAS, percent — an upward-sloping investment-grade curve.
BUCKETS_PCT = {
    "BAMLC1A0C13Y": 0.55,
    "BAMLC2A0C35Y": 0.72,
    "BAMLC3A0C57Y": 0.88,
    "BAMLC4A0C710Y": 1.02,
    "BAMLC7A0C1015Y": 1.15,
    "BAMLC8A0C15PY": 1.21,
}
RATINGS_PCT = {
    "BAMLC0A1CAAA": 0.38,
    "BAMLC0A2CAA": 0.52,
    "BAMLC0A3CA": 0.71,
    "BAMLC0A4CBBB": 1.02,
    "BAMLH0A1HYBB": 1.85,
    "BAMLH0A2HYB": 2.95,
    "BAMLH0A3HYC": 7.9,
}


@pytest.fixture
def store(monkeypatch):
    state = {"curves": {**USD_PCT, **BUCKETS_PCT, **RATINGS_PCT}}

    def snapshot(table, ids):
        if not all(i in state["curves"] for i in ids):
            return None
        return AS_OF, {i: state["curves"][i] for i in ids}

    monkeypatch.setattr(db, "rates_curve_snapshot", snapshot)
    monkeypatch.setattr(
        db, "rates_latest", lambda t, ids: {i: (AS_OF, 1.0) for i in ids}
    )
    monkeypatch.setattr(
        db,
        "rates_history",
        lambda t, sid, since: [
            (since + timedelta(days=k), 1.0 + k / 100) for k in range(3)
        ],
    )
    for cache in (
        credit_router._SPREADS_CACHE,
        credit_router._HISTORY_CACHE,
        credit_router._FUNDAMENTALS_CACHE,
        credit_router._STRUCTURAL_CACHE,
        credit_router._COMPANIES_CACHE,
    ):
        cache._store.clear()
    return state


@pytest.fixture
def client():
    return TestClient(app)


# ── Market spreads ───────────────────────────────────────────────────────────


def test_the_hazard_curve_reprices_every_bucket(store):
    o = credit.spreads_overview(0.4)
    assert o.hazard is not None
    assert o.hazard.max_repricing_error_bp < 1e-6
    assert [q.tenor for q in o.term_structure] == [2.0, 4.0, 6.0, 8.5, 12.5, 20.0]
    assert all(h >= 0 for h in o.hazard.hazards)
    surv = o.hazard.survival
    assert surv[0] == 1.0 and all(a >= b for a, b in zip(surv, surv[1:]))


def test_the_first_hazard_is_close_to_the_credit_triangle(store):
    o = credit.spreads_overview(0.4)
    s = BUCKETS_PCT["BAMLC1A0C13Y"] / 100
    assert o.hazard.hazards[0] == pytest.approx(s / 0.6, rel=0.01)


def test_ratings_default_probabilities_rise_down_the_scale_and_with_horizon(store):
    o = credit.spreads_overview(0.4)
    five_year = [r.default_probabilities[1] for r in o.ratings]
    assert five_year == sorted(five_year)
    for r in o.ratings:
        assert r.default_probabilities == sorted(r.default_probabilities)
        assert r.hazard == pytest.approx(
            -math.log(1 - r.default_probabilities[0]), rel=1e-12
        )


def test_a_higher_recovery_needs_more_default_risk_for_the_same_spread(store):
    low = credit.spreads_overview(0.2).hazard.hazards[0]
    high = credit.spreads_overview(0.6).hazard.hazards[0]
    assert high == pytest.approx(low * 0.8 / 0.4, rel=0.01)


def test_an_inverted_curve_is_reported_not_drawn(store, client):
    store["curves"]["BAMLC2A0C35Y"] = 0.05  # 5bp after 55bp: no non-negative hazard
    r = client.get("/api/credit/spreads/overview")
    assert r.status_code == 200
    body = r.json()
    assert body["hazard"] is None
    assert "could not be bootstrapped" in body["hazard_unavailable"]
    assert len(body["term_structure"]) == 6


def test_overview_route_and_methodology(store, client):
    body = client.get("/api/credit/spreads/overview?recovery=0.4").json()
    assert body["recovery"] == 0.4
    assert [r["label"] for r in body["ratings"]][0] == "AAA"
    assert any(
        s["title"] == "From spreads to a hazard curve" for s in body["methodology"]
    )
    assert client.get("/api/credit/spreads/overview?recovery=0.95").status_code == 422


def test_history_whitelists_series(store, client):
    ok = client.get("/api/credit/spreads/history?series_id=BAMLC0A4CBBB&years=2")
    assert ok.status_code == 200 and ok.json()["kind"] == "spread"
    assert (
        client.get("/api/credit/spreads/history?series_id=DAAA").json()["kind"]
        == "yield"
    )
    assert (
        client.get("/api/credit/spreads/history?series_id=DROP_TABLE").status_code
        == 404
    )


# ── Fundamentals ─────────────────────────────────────────────────────────────

FY24, FY23 = date(2024, 12, 31), date(2023, 12, 31)
Q2_25 = date(2025, 6, 30)
K24, K25 = "0000000001-25-000001", "0000000001-25-000002"
Q25 = "0000000001-25-000100"


def _flow(concept, end, value, accession, filed, days=365, form="10-K"):
    return db.SecFact(
        concept,
        "USD",
        end - timedelta(days=days),
        end,
        days,
        value,
        form,
        filed,
        accession,
    )


def _inst(concept, end, value, accession, filed, form="10-K"):
    return db.SecFact(concept, "USD", None, end, 0, value, form, filed, accession)


FACTS = [
    # Revenue tagged under the second candidate only.
    _flow(
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        FY24,
        1000.0,
        K24,
        date(2025, 2, 1),
    ),
    _flow(
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        FY23,
        900.0,
        K24,
        date(2025, 2, 1),
    ),
    # FY23 net income first reported 80, restated to 85 in the next 10-K.
    _flow("NetIncomeLoss", FY23, 80.0, "0000000001-24-000001", date(2024, 2, 1)),
    _flow("NetIncomeLoss", FY23, 85.0, K24, date(2025, 2, 1)),
    _flow("NetIncomeLoss", FY24, 120.0, K24, date(2025, 2, 1)),
    _flow("OperatingIncomeLoss", FY24, 200.0, K24, date(2025, 2, 1)),
    _flow("InterestExpense", FY24, 20.0, K24, date(2025, 2, 1)),
    _flow("DepreciationDepletionAndAmortization", FY24, 50.0, K24, date(2025, 2, 1)),
    _flow(
        "NetCashProvidedByUsedInOperatingActivities", FY24, 300.0, K24, date(2025, 2, 1)
    ),
    _flow(
        "PaymentsToAcquirePropertyPlantAndEquipment", FY24, 70.0, K24, date(2025, 2, 1)
    ),
    _inst("Assets", FY24, 5000.0, K24, date(2025, 2, 1)),
    _inst("Assets", FY23, 4800.0, K24, date(2025, 2, 1)),
    _inst("StockholdersEquity", FY24, 2000.0, K24, date(2025, 2, 1)),
    _inst("CashAndCashEquivalentsAtCarryingValue", FY24, 400.0, K24, date(2025, 2, 1)),
    # Debt: LongTermDebt includes the current portion.
    _inst("LongTermDebt", FY24, 1500.0, K24, date(2025, 2, 1)),
    _inst("LongTermDebtCurrent", FY24, 100.0, K24, date(2025, 2, 1)),
    _inst("ShortTermBorrowings", FY24, 50.0, K24, date(2025, 2, 1)),
    _inst(
        "CommercialPaper", FY24, 30.0, K24, date(2025, 2, 1)
    ),  # inside borrowings: ignored
    # A later 10-Q: quarterly balance sheet, the one the structural model reads.
    _inst("Assets", Q2_25, 5100.0, Q25, date(2025, 8, 1), form="10-Q"),
    _inst("LongTermDebtNoncurrent", Q2_25, 1300.0, Q25, date(2025, 8, 1), form="10-Q"),
    _inst("DebtCurrent", Q2_25, 200.0, Q25, date(2025, 8, 1), form="10-Q"),
    db.SecFact(
        "CommonStockSharesOutstanding",
        "shares",
        None,
        Q2_25,
        0,
        100.0,
        "10-Q",
        date(2025, 8, 1),
        Q25,
    ),
]

COMPANY = db.SecCompany(
    1, ("ACME",), "Acme Corp", "3571", "Electronic Computers", date(2025, 8, 1)
)


@pytest.fixture
def sec(monkeypatch, store):
    state = {"company": COMPANY}
    monkeypatch.setattr(
        db, "sec_company", lambda t: state["company"] if t.upper() == "ACME" else None
    )
    monkeypatch.setattr(
        db,
        "sec_facts",
        lambda cik, concepts: [f for f in FACTS if f.concept in concepts],
    )
    monkeypatch.setattr(
        db,
        "sec_filings",
        lambda cik, limit=12: [
            db.SecFiling(
                K24, "10-K", date(2025, 2, 1), FY24, f"https://sec.example/{K24}"
            ),
        ],
    )
    monkeypatch.setattr(
        db,
        "sec_filing_urls",
        lambda cik, accs: {a: f"https://sec.example/{a}" for a in accs},
    )
    monkeypatch.setattr(db, "sec_companies", lambda: [state["company"]])
    # Daily closes with a known realised vol: alternating ±1 % log returns.
    closes, p = [], 100.0
    for k in range(260):
        closes.append((TODAY - timedelta(days=260 - k), p))
        p *= math.exp(0.01 if k % 2 == 0 else -0.01)
    monkeypatch.setattr(db, "price_history", lambda t, since=None: closes)
    return state


def _rows(s):
    return {r.key: r for r in s.rows + s.ratios}


def test_annual_periods_are_fiscal_year_ends_from_10k(sec):
    s = fundamentals.statements("ACME", "annual", 6)
    assert s.periods == [FY24, FY23]


def test_candidates_in_order_and_latest_restatement(sec):
    rows = _rows(fundamentals.statements("ACME", "annual", 6))
    rev = rows["revenue"].values[0]
    assert rev.value == 1000.0
    assert rev.concept == "RevenueFromContractWithCustomerExcludingAssessedTax"
    ni23 = rows["net_income"].values[1]
    assert ni23.value == 85.0 and ni23.accession == K24


def test_derived_lines_follow_their_formulas(sec):
    rows = _rows(fundamentals.statements("ACME", "annual", 6))
    assert rows["long_term_debt"].values[0].value == 1400.0  # 1500 − 100
    assert rows["short_term_debt"].values[0].value == 150.0  # 100 + 50, paper ignored
    assert rows["total_debt"].values[0].value == 1550.0
    assert rows["net_debt"].values[0].value == 1150.0
    assert rows["fcf"].values[0].value == 230.0
    assert rows["ebitda"].values[0].value == 250.0
    assert rows["interest_coverage"].values[0].value == 10.0
    assert rows["net_margin"].values[0].value == pytest.approx(0.12)
    assert (
        rows["gross_margin"].values[0] is None
    )  # no gross profit tagged: nothing invented


def test_fundamentals_route_links_every_figure(sec, client):
    body = client.get("/api/credit/companies/acme/fundamentals").json()
    assert body["company"]["name"] == "Acme Corp"
    ltd = next(r for r in body["rows"] if r["key"] == "long_term_debt")
    cell = ltd["values"][0]
    assert cell["formula"] == "LongTermDebt − LongTermDebtCurrent"
    assert {s["concept"] for s in cell["sources"]} == {
        "LongTermDebt",
        "LongTermDebtCurrent",
    }
    assert all(s["url"].startswith("https://sec.example/") for s in cell["sources"])
    assert body["filings"][0]["form"] == "10-K"
    assert client.get("/api/credit/companies/NOPE/fundamentals").status_code == 404


# ── Structural model ─────────────────────────────────────────────────────────


def test_structural_reads_the_latest_balance_sheet_and_reproduces_equity(sec):
    r = credit.structural("ACME")
    assert r.unavailable is None
    i = r.inputs
    assert i.balance_sheet.period_end == Q2_25
    assert i.default_point == pytest.approx(200.0 + 0.5 * 1300.0)
    assert i.equity_vol == pytest.approx(0.01 * math.sqrt(252), rel=0.01)
    # The calibrated firm prices the observed equity back (Merton: E = V − B).
    ts = qm.merton_term_structure(
        r.asset_value, r.asset_vol, i.default_point, i.rate, [1.0]
    )
    spread = ts["credit_spread"][0]
    debt = i.default_point * math.exp(-(i.rate + spread))
    assert r.asset_value - debt == pytest.approx(i.equity_value, rel=1e-9)
    assert r.leverage == pytest.approx(i.default_point / r.asset_value)
    assert all(s >= 0 for s in r.spreads)
    assert r.default_probabilities == sorted(r.default_probabilities)


def test_financials_are_not_modelled(sec, client):
    sec["company"] = db.SecCompany(
        1, ("ACME",), "Acme Bank", "6021", "National Commercial Banks", date(2025, 8, 1)
    )
    body = client.get("/api/credit/companies/ACME/structural").json()
    assert body["asset_value"] is None
    assert "financial company" in body["unavailable"]


def test_companies_route(sec, client):
    body = client.get("/api/credit/companies").json()
    assert body["companies"][0]["tickers"] == ["ACME"]
