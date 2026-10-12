"""Counterparties and netting sets of a portfolio: the ledger's referential
rules, the examples, and that who a trade faces does not move its mark."""

from __future__ import annotations

from datetime import date

import pytest
from fastapi.testclient import TestClient

from api.app import portfolio_counterparties as cp
from api.app import portfolio_ledger as L
from api.app import portfolio_valuation as pv
from api.app.main import app
from api.app.portfolio_schemas import (
    Counterparty,
    DerivativeSpec,
    EquitySpec,
    Instrument,
    NettingSet,
    Portfolio,
)
from api.app.xva import RATINGS


def _option(netting_set_id=None) -> Instrument:
    return Instrument(
        id="opt",
        netting_set_id=netting_set_id,
        spec=DerivativeSpec(
            product="vanilla",
            params={"strike": 100.0, "is_call": True},
            underlying="AAA",
            expiry=date(2027, 6, 18),
        ),
    )


def _pf(*instruments: Instrument, counterparties=None) -> Portfolio:
    return Portfolio(
        id="p",
        version=2,
        instruments=list(instruments),
        counterparties=cp.examples() if counterparties is None else counterparties,
    )


def test_a_derivative_can_be_traded_under_a_netting_set_of_the_portfolio():
    pf = _pf(_option("example-dealer-vm"))
    L.validate(pf)
    party, netting_set = cp.netting_set(pf, "example-dealer-vm")
    assert party.id == "example-dealer" and netting_set.csa is not None


def test_an_unknown_netting_set_is_refused():
    with pytest.raises(ValueError, match="unknown netting set"):
        L.validate(_pf(_option("gone")))
    # ... which is what removing a counterparty still in use comes to.
    with pytest.raises(ValueError, match="unknown netting set"):
        L.validate(_pf(_option("example-dealer-vm"), counterparties=[]))


def test_a_listed_instrument_belongs_to_no_netting_set():
    share = Instrument(
        id="EQ:AAA", spec=EquitySpec(ticker="AAA"), netting_set_id="example-dealer-vm"
    )
    with pytest.raises(ValueError, match="listed instrument"):
        L.validate(_pf(share))


def test_ids_are_unique_across_the_portfolio():
    twice = [
        Counterparty(id="a", name="A", netting_sets=[NettingSet(id="n", name="N")]),
        Counterparty(id="b", name="B", netting_sets=[NettingSet(id="n", name="N")]),
    ]
    with pytest.raises(ValueError, match="duplicate netting set"):
        L.validate(_pf(counterparties=twice))
    with pytest.raises(ValueError, match="duplicate counterparty"):
        L.validate(_pf(counterparties=[twice[0], twice[0]]))


def test_the_netting_set_is_not_part_of_what_a_mark_is_stored_under():
    assert pv.MarkStore.key(_option()) == pv.MarkStore.key(_option("example-fund-vm"))


def test_the_examples_are_well_formed_and_served():
    L.validate(_pf())
    for c in cp.EXAMPLES:
        assert c.rating in RATINGS and c.netting_sets
    # Copies: editing one in a portfolio must not edit the examples.
    cp.examples()[0].name = "changed"
    assert cp.EXAMPLES[0].name != "changed"
    r = TestClient(app).get("/api/portfolio-valuation/counterparty-examples")
    assert r.status_code == 200
    assert [c["id"] for c in r.json()] == [c.id for c in cp.EXAMPLES]


def test_a_portfolio_stored_before_counterparties_still_loads():
    pf = Portfolio(**{"id": "old", "version": 2, "instruments": [], "transactions": []})
    assert pf.counterparties == []
