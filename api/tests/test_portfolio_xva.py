"""xVA of a netting set of a portfolio — `portfolio_xva.py` and
`/api/xva/portfolio`, on the real C++ library and the fake store of the xVA
tests.

Properties: a portfolio that recomposes a teaching portfolio of the xVA page
gives its figures to the bit; the counterparty's rating and the netting set's
collateral agreement are the ledger's; what the engine cannot hold is left
out and said; a position on the other side is the opposite trade.
"""

from __future__ import annotations

from datetime import date

import pytest

from api.app import portfolio_counterparties as cp
from api.app import portfolio_xva
from api.app.portfolio_schemas import (
    DerivativeSpec,
    Instrument,
    Portfolio,
    RateFxSpec,
    Trade,
    add_years,
)
from api.tests.test_xva import AS_OF, PATHS, client, run, store  # noqa: F401

NOTIONAL = 10_000_000.0


def _swap(rate: float, years=10, start=AS_OF, payer=True, **more) -> RateFxSpec:
    return RateFxSpec(
        product="swap",
        currency="USD",
        notional=NOTIONAL,
        payer=payer,
        start=start,
        maturity=add_years(start, years),
        fixed_rate=rate,
        **more,
    )


def _pf(*specs, netting_set="example-corporate-isda", quantity=1.0) -> Portfolio:
    instruments = [
        Instrument(id=f"i{k}", label=f"Trade {k}", spec=s, netting_set_id=netting_set)
        for k, s in enumerate(specs)
    ]
    return Portfolio(
        id="p",
        version=2,
        counterparties=cp.examples(),
        instruments=instruments,
        transactions=[
            Trade(
                id=f"t{k}",
                instrument_id=i.id,
                trade_date=min(AS_OF, date(2025, 1, 2)),
                quantity=quantity,
                price=0.0,
            )
            for k, i in enumerate(instruments)
        ],
    )


def post(pf: Portfolio, netting_set="example-corporate-isda", **body):
    return client.post(
        "/api/xva/portfolio",
        json={
            "portfolio": pf.model_dump(mode="json"),
            "netting_set_id": netting_set,
            "paths": PATHS,
            **body,
        },
    )


def ledger(pf: Portfolio, netting_set="example-corporate-isda", **body) -> dict:
    r = post(pf, netting_set, **body)
    assert r.status_code == 200, r.text
    return r.json()


def test_the_teaching_swap_recomposed_in_a_portfolio_gives_the_same_figures():
    page = run(
        portfolio="single_swap",
        counterparty_rating="BBB",
        # The sector of the ledger's counterparty weighs its CVA capital.
        capital={"sector": "basic_materials_energy_industrials"},
    )
    par = page["trades"][0]["fixed_rate"]
    mine = ledger(_pf(_swap(par)))
    assert mine["portfolio"] == "ledger"
    assert mine["portfolio_label"] == (
        "Meridian Industries (corporate): ISDA, uncollateralised"
    )
    for key in ("exposure", "adjustments", "capital", "risk"):
        assert mine[key] == page[key], key
    assert mine["trades"][0]["description"] == "Trade 0"
    assert mine["trades"][0]["fixed_rate"] == par


def test_the_counterparty_and_the_collateral_are_those_of_the_ledger():
    page = run(portfolio="single_swap", counterparty_rating="BBB")
    par = page["trades"][0]["fixed_rate"]
    # The dealer is rated A and posts variation margin.
    dealer = ledger(
        _pf(_swap(par), netting_set="example-dealer-vm"), "example-dealer-vm"
    )
    assert dealer["market"]["counterparty"]["rating"] == "A"
    assert dealer["exposure_uncollateralised"] is not None
    assert dealer["exposure"]["epe"] < 0.5 * dealer["exposure_uncollateralised"]["epe"]
    assert abs(dealer["adjustments"]["cva"]["value"]) < abs(
        page["adjustments"]["cva"]["value"]
    )


def test_a_swap_off_the_market_is_worth_what_its_rate_makes_it():
    page = run(portfolio="single_swap")
    par = page["trades"][0]["fixed_rate"]
    cheap = ledger(_pf(_swap(par - 0.01)))  # pays 1 % less than the market
    assert cheap["value_today"] > 0.05 * NOTIONAL
    assert cheap["exposure"]["epe"] > page["exposure"]["epe"]
    # The other side of the same contract: the exposures swap places.
    other = ledger(_pf(_swap(par - 0.01), quantity=-1.0))
    assert other["value_today"] == pytest.approx(-cheap["value_today"])
    assert other["exposure"]["ene"] == pytest.approx(
        [-x for x in cheap["exposure"]["ee"]]
    )


def test_a_swap_already_running_is_simulated_from_its_next_coupon_and_says_so():
    started = date(2025, 3, 31)  # next coupon 2027-03-31, then 8 more
    body = ledger(_pf(_swap(0.045, years=10, start=started)))
    (trade,) = body["trades"]
    assert trade["tenor"] == 8 and 0.4 < trade["start"] < 0.6
    assert any(
        "coupon in progress" in w and "2027-03-31" in w for w in body["warnings"]
    )


def test_what_the_engine_cannot_hold_is_left_out_and_said():
    option = DerivativeSpec(
        product="vanilla",
        params={"strike": 100.0, "is_call": True},
        underlying="AAA",
        expiry=date(2027, 6, 18),
    )
    body = ledger(_pf(_swap(0.047), option))
    assert len(body["trades"]) == 1
    assert any("equity derivative" in w for w in body["warnings"])
    # Nothing left to simulate is an error, not an empty result.
    r = post(_pf(option))
    assert r.status_code == 422 and "no trade the exposure engine" in r.text
    r = post(_pf(_swap(0.047)), "nowhere")
    assert r.status_code == 422 and "no netting set" in r.text
    # A trade under another netting set is not in this one.
    r = post(_pf(_swap(0.047)), "example-dealer-vm")
    assert r.status_code == 422


def test_a_closed_position_is_not_in_the_netting_set():
    pf = _pf(_swap(0.047), _swap(0.03, years=5))
    pf.transactions.append(
        Trade(
            id="close",
            instrument_id="i1",
            trade_date=date(2025, 6, 2),
            quantity=-1.0,
            price=0.0,
        )
    )
    assert [t["description"] for t in ledger(pf)["trades"]] == ["Trade 0"]


def test_trades_in_two_currencies_net_in_dollars():
    forward = RateFxSpec(
        product="fx_forward",
        currency="EUR",
        notional=9_000_000.0,
        payer=True,
        start=AS_OF,
        maturity=add_years(AS_OF, 2),
        fixed_rate=1.20,
    )
    ccs = RateFxSpec(
        product="cross_currency_swap",
        currency="EUR",
        notional=9_000_000.0,
        payer=False,
        start=AS_OF,
        maturity=add_years(AS_OF, 5),
        fixed_rate=0.047,
        foreign_rate=0.025,
        domestic_notional=10_400_000.0,
    )
    body = ledger(_pf(_swap(0.047), forward, ccs))
    kinds = [t["kind"] for t in body["trades"]]
    assert kinds == ["swap", "fx_forward", "cross_currency_swap"]
    assert body["trades"][1]["fixed_rate"] == 1.20
    assert body["trades"][2]["foreign_rate"] == 0.025
    assert body["market"]["foreign"] is not None and body["device"] == "cpu"
    assert body["adjustments"]["cva"]["value"] < 0


def test_the_audit_record_holds_the_trades_and_not_the_portfolio(store):
    ledger(_pf(_swap(0.047)), seed=5)
    ((event_type, payload),) = store["events"]
    assert event_type == "xva.valuation"
    assert payload.request["portfolio"] == "ledger" and payload.request["seed"] == 5
    assert payload.request["counterparty_rating"] == "BBB"
    (trade,) = payload.request["trades"]
    assert trade["kind"] == "swap" and trade["fixed_rate"] == 0.047
    assert "owner" not in str(payload.request)


def test_the_ledger_netting_set_is_not_a_portfolio_of_the_xva_page():
    r = client.post("/api/xva/netting-set", json={"portfolio": "ledger"})
    assert r.status_code == 422
    listed = client.get("/api/xva/portfolios").json()["portfolios"]
    assert "ledger" not in [p["id"] for p in listed]
