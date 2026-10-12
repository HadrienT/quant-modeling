"""xVA of a netting set of a portfolio — `portfolio_xva.py` and
`/api/xva/portfolio`, on the real C++ library and the fake store of the xVA
tests.

Properties: a portfolio that recomposes a teaching portfolio of the xVA page
gives its figures to the bit; the counterparty's rating and the netting set's
collateral agreement are the ledger's; what the engine cannot hold is left
out and said; a position on the other side is the opposite trade.
"""

from __future__ import annotations

import math
from datetime import date, timedelta

import pandas as pd
import pytest

from api.app import db

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
from api.tests import test_xva
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


def _option(strike=100.0, call=True, underlying="AAA", years=2, **more):
    return DerivativeSpec(
        **{
            "product": "vanilla",
            "params": {"strike": strike, "is_call": call},
            "underlying": underlying,
            "expiry": add_years(AS_OF, years),
            "currency": "USD",
            **more,
        }
    )


SHARE_DAYS = [d.date() for d in pd.bdate_range(date(2019, 1, 1), AS_OF)]


def _rate_history() -> pd.Series:
    """A daily rate around 4 %, in percent, up to the valuation date."""
    days = pd.bdate_range(date(2016, 1, 4), AS_OF)
    values, r = [], 4.0
    for k in range(len(days)):
        shock = math.sin(12.9898 * k) * 43758.5453
        r += 0.0012 * (2.5 - r) + 0.05 * (2.0 * (shock - math.floor(shock)) - 1.0)
        values.append(r)
    return pd.Series(values, index=days)


def SHARE(ticker, since):
    """The closes of the two fake shares, ending at 100."""
    if ticker not in ("AAA", "BBB"):
        return []
    salt = 1.0 if ticker == "AAA" else 2.3
    out, level = [], 100.0
    for k, d in enumerate(SHARE_DAYS):
        level *= math.exp(0.012 * math.sin(salt * k * 1.7) * math.cos(k / 7.0))
        out.append((d, level))
    scale = 100.0 / out[-1][1]
    return [(d, v * scale) for d, v in out if d >= since]


@pytest.fixture(autouse=True)
def shares(monkeypatch, store):  # noqa: F811 — after the store it completes
    """Two listed shares around 100, with five years of closes; no option
    chain (the realised volatility stands in) and a 1.5 % dividend yield."""
    monkeypatch.setattr(
        db, "fred_series", lambda series_id, since=None: _rate_history()
    )

    monkeypatch.setattr(db, "price_history", SHARE)
    monkeypatch.setattr(db, "ticker_currency", lambda t: "USD")
    monkeypatch.setattr(
        db,
        "dividend_yield_history",
        lambda t, since: [(AS_OF - timedelta(days=5), 0.015)],
    )
    monkeypatch.setattr(db, "options_snapshot_date_on_or_before", lambda t, d: None)
    monkeypatch.setattr(db, "rates_curve_history", lambda table, ids, since: [])


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
    american = _option(product="american_vanilla")
    body = ledger(_pf(_swap(0.047), american))
    assert len(body["trades"]) == 1
    assert any("European options only" in w for w in body["warnings"])
    # Nothing left to simulate is an error, not an empty result.
    r = post(_pf(american))
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


# ── Lot X14: equity options ──────────────────────────────────────────────────


def test_a_bought_equity_option_is_an_asset_until_it_expires():
    body = ledger(_pf(_option(105.0), quantity=1000.0))
    (trade,) = body["trades"]
    assert (trade["kind"], trade["fixed_rate"], trade["quantity"]) == (
        "equity_option",
        105.0,
        1000.0,
    )
    assert trade["value_today"] > 0 and 1.99 < trade["maturity"] < 2.01
    exposure = body["exposure"]
    # It can only be owed to the bank: no negative exposure, no DVA.
    assert max(exposure["ee"]) > 0 and min(exposure["ene"]) == 0.0
    assert body["adjustments"]["cva"]["value"] < 0
    assert body["adjustments"]["dva"]["value"] == 0.0
    # Until the expiry its discounted expected exposure is its price.
    discounted = body["exposure"]["discounted_ee"][0]
    assert discounted == pytest.approx(
        body["value_today"], rel=5 * 1.96 / math.sqrt(PATHS)
    )
    assert body["device"] == "cpu" and body["risk"] is None


def test_a_sold_equity_option_has_no_exposure():
    body = ledger(_pf(_option(95.0, call=False), quantity=-500.0))
    assert body["value_today"] < 0
    assert max(body["exposure"]["ee"]) == 0.0
    assert body["adjustments"]["cva"]["value"] == 0.0
    assert body["adjustments"]["dva"]["value"] > 0


def test_the_share_says_what_it_rests_on():
    body = ledger(_pf(_option(100.0), quantity=100.0))
    e = body["market"]["equity"]
    assert e["ticker"] == "AAA" and e["spot"] == pytest.approx(100.0)
    assert e["dividend_yield"] == 0.015
    # No option chain: the realised volatility, a proxy, and it says so.
    assert e["volatility_proxied"] and "realised vol" in e["volatility_source"]
    assert 0.02 < e["market_volatility"] < 1.0
    assert e["calibration_strike"] == 100.0
    assert (
        e["correlation"]["low"] < e["correlation"]["value"] < e["correlation"]["high"]
    )
    # The share's own volatility differs from the option's: rates move too.
    assert e["spot_volatility"] != e["market_volatility"]
    assert e["spot_volatility"] == pytest.approx(e["market_volatility"], rel=0.05)
    warned = " ".join(body["warnings"])
    assert "historical estimate" in warned and "a proxy" in warned
    assert "Equity options in the netting set" in [
        m["title"] for m in body["methodology"]
    ]
    assert body["capital"] is not None  # SA-CCR describes an equity option


def test_equity_options_net_with_dollar_swaps():
    alone = ledger(_pf(_swap(0.047)))
    both = ledger(_pf(_swap(0.047), _option(100.0, call=False)))
    assert [t["kind"] for t in both["trades"]] == ["swap", "equity_option"]
    # The swap is the same trade in both netting sets.
    assert both["trades"][0]["value_today"] == pytest.approx(
        alone["trades"][0]["value_today"]
    )
    assert both["value_today"] > alone["value_today"]


def test_one_share_per_netting_set_and_no_second_currency_with_it():
    two = ledger(_pf(_option(100.0), _option(100.0, underlying="BBB"), _option(110.0)))
    assert [t["fixed_rate"] for t in two["trades"]] == [100.0, 110.0]
    warned = " ".join(two["warnings"])
    assert "those on BBB are left out" in warned and "without the smile" in warned
    forward = RateFxSpec(
        product="fx_forward",
        currency="EUR",
        notional=9_000_000.0,
        payer=True,
        start=AS_OF,
        maturity=add_years(AS_OF, 2),
        fixed_rate=1.20,
    )
    mixed = ledger(_pf(forward, _option(100.0)))
    assert [t["kind"] for t in mixed["trades"]] == ["fx_forward"]
    assert any("its equity options are left out" in w for w in mixed["warnings"])


def test_a_share_without_history_is_an_explicit_error():
    r = post(_pf(_option(100.0, underlying="ZZZ")))
    assert r.status_code == 503 and "No close of ZZZ" in r.text


# ── Sensitivities of a netting set, by adjoint differentiation ───────────────


def risks(pf: Portfolio, netting_set="example-corporate-isda", **body):
    return client.post(
        "/api/xva/portfolio/sensitivities",
        json={
            "portfolio": pf.model_dump(mode="json"),
            "netting_set_id": netting_set,
            "paths": PATHS,
            **body,
        },
    )


def test_the_sensitivities_of_the_teaching_swap_recomposed_are_the_same():
    page = client.post(
        "/api/xva/sensitivities",
        json={
            "portfolio": "single_swap",
            "paths": PATHS,
            "sector": "basic_materials_energy_industrials",
        },
    ).json()
    par = run(portfolio="single_swap")["trades"][0]["fixed_rate"]
    r = risks(_pf(_swap(par)))
    assert r.status_code == 200, r.text
    mine = r.json()
    assert mine["portfolio"] == "ledger"
    for key in ("adjustments", "swap_rates", "swaption_vols", "sa_cva"):
        assert mine[key] == page[key], key


def test_adjoint_sensitivities_of_a_trade_off_the_market_match_bumped_quotes():
    # Off the market, starting in a year, on the other side: nothing the
    # teaching portfolios have.
    pf = _pf(
        _swap(0.040, years=7, start=add_years(AS_OF, 1), payer=False),
        _swap(0.052, years=4),
        quantity=1.0,
    )
    base = risks(pf, seed=3).json()
    tenors = [q["tenor"] for q in base["swap_rates"]]
    for tenor in (5, 10):
        reported = base["swap_rates"][tenors.index(float(tenor))]
        bumped = {}
        quoted = test_xva.PAR[tenor]
        try:
            for sign in (+1, -1):
                test_xva.PAR[tenor] = quoted + sign * 1e-4
                bumped[sign] = risks(pf, seed=3).json()["adjustments"]
        finally:
            test_xva.PAR[tenor] = quoted
        for name in ("cva", "dva"):
            finite = (bumped[1][name]["value"] - bumped[-1][name]["value"]) / 2e-4
            assert reported[name]["value"] == pytest.approx(
                finite, rel=0.03, abs=3 * reported[name]["error"]
            ), (tenor, name)
    # Both trades end before 10 years... the 8-year receiver does not: the
    # 10-year quote matters, the 30-year one does not.
    assert base["swap_rates"][tenors.index(10.0)]["cva"]["value"] != 0.0
    assert base["swap_rates"][-1]["cva"]["value"] == 0.0


def test_a_netting_set_with_a_share_has_the_sensitivities_of_both_factors():
    pf = _pf(_swap(0.047), _option(100.0), quantity=1.0)
    pf.transactions[1].quantity = 3000.0  # 3 000 calls next to the swap
    r = risks(pf, seed=5)
    assert r.status_code == 200, r.text
    body = r.json()
    by_label = {q["label"]: q for q in body["second_factor"]}
    assert set(by_label) == {
        "dividend yield of AAA",
        "price of AAA",
        "market volatility of AAA",
        "correlation of the dollar rate and AAA",
    }
    # Bought calls: the bank is owed more when the share rises or moves more
    # (the CVA, a cost, grows), less when it pays more dividends.
    assert by_label["price of AAA"]["cva"]["value"] < 0
    assert by_label["market volatility of AAA"]["cva"]["value"] < 0
    assert by_label["dividend yield of AAA"]["cva"]["value"] > 0
    assert by_label["dividend yield of AAA"]["level"] == pytest.approx(0.015)
    # The delta against the share's price moved in the store, same draws.
    valued = {}
    for sign in (+1, -1):
        scale = 1.0 + sign * 0.001

        def moved(ticker, since, scale=scale):
            return [(d, v * scale) for d, v in SHARE(ticker, since)]

        original = db.price_history
        db.price_history = moved
        try:
            valued[sign] = risks(pf, seed=5).json()["adjustments"]["cva"]["value"]
        finally:
            db.price_history = original
    finite = (valued[1] - valued[-1]) / (0.002 * by_label["price of AAA"]["level"])
    reported = by_label["price of AAA"]["cva"]
    assert reported["value"] == pytest.approx(
        finite, rel=0.05, abs=3 * reported["error"]
    )
    assert body["sa_cva"] is None and "a share" in body["sa_cva_unavailable"]
    # The swap rates still have theirs.
    assert any(q["cva"]["value"] != 0.0 for q in body["swap_rates"])


def test_sensitivities_say_what_they_do_not_cover():
    r = risks(_pf(_swap(0.047), netting_set="example-dealer-im"), "example-dealer-im")
    assert r.status_code == 422 and "initial margin" in r.text
    r = risks(_pf(_option(100.0, product="american_vanilla")))
    assert r.status_code == 422 and "no such trade" in r.text
    # A swap already running: the coupon in progress is said to be left out.
    body = risks(_pf(_swap(0.045, start=date(2025, 3, 31)))).json()
    assert any("coupon in progress" in w for w in body["warnings"])


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
