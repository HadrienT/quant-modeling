"""xVA of a netting set of a portfolio (blueprint/wp/23-xva.md, lot X13).

The Portfolio page lets one compose netting sets: counterparties, collateral
agreements, and the trades under each (portfolio_counterparties.py,
portfolio_rates.py). This module turns one of them into what the exposure
engine simulates — the trades held today, with the life they have left and
the rates of their contracts — and runs the computation of the xVA page
(xva.py) on it, with the counterparty's rating, sector and recovery and the
netting set's collateral agreement.

What the engine cannot hold is left out, and said so in the warnings of the
response, never approximated in silence.
"""

from __future__ import annotations

from datetime import date
from typing import List, Literal, Optional, Tuple

from pydantic import BaseModel, Field

from . import portfolio_counterparties, portfolio_ledger, portfolio_rates, xva
from .portfolio_schemas import (
    DerivativeSpec,
    Instrument,
    Portfolio,
    RateFxSpec,
    whole_years,
)
from .xva_schemas import CapitalInput, Rating, XvaRequest, XvaResponse


class PortfolioXvaRequest(BaseModel):
    """A netting set of the portfolio, and what is the bank's own: its
    rating, its funding spreads, the return it wants on capital. The
    counterparty's side comes from the portfolio."""

    portfolio: Portfolio
    netting_set_id: str
    own_rating: Rating = "A"
    borrowing_spread: float = Field(default=0.0, ge=0, le=0.1)
    lending_spread: float = Field(default=0.0, ge=0, le=0.1)
    cost_of_capital: float = Field(default=0.10, ge=0, le=0.5)
    wrong_way_risk: float = Field(default=0.0, ge=-30.0, le=30.0)
    paths: int = Field(default=10000, ge=1000, le=50000)
    seed: int = Field(default=42, ge=0, le=2**31 - 1)
    pfe_confidence: float = Field(default=0.95, gt=0.5, lt=1.0)
    device: Literal["auto", "cpu", "gpu"] = "auto"


def _rate_fx(
    inst: Instrument, spec: RateFxSpec, quantity: float, d: date
) -> Tuple[Optional[xva._Spec], Optional[str]]:
    """The trade as the engine takes it on `d`, or why it is left out."""
    name = inst.label or inst.id
    t = portfolio_rates.years
    if spec.product == "fx_forward":
        if d >= spec.maturity:
            return None, f"{name}: delivered, nothing is left to simulate."
        return (
            xva._Spec(
                name,
                "fx_forward",
                spec.payer,
                t(d, spec.maturity),
                spec.notional,
                quantity=quantity,
                rate=spec.fixed_rate,
            ),
            None,
        )
    if spec.product == "swaption":
        if d >= spec.start:
            return None, (
                f"{name}: expired. If it was exercised, book the swap entered as a "
                "trade of its own."
            )
        return (
            xva._Spec(
                name,
                "swaption",
                spec.payer,
                float(whole_years(spec.start, spec.maturity)),
                spec.notional,
                start=t(d, spec.start),
                quantity=quantity,
                currency=spec.currency,
                rate=spec.fixed_rate,
            ),
            None,
        )
    left = [p for p in portfolio_rates.schedule(spec) if p > d]
    if not left:
        return None, f"{name}: matured, nothing is left to simulate."
    if spec.product == "cross_currency_swap":
        if d > spec.start and d not in portfolio_rates.schedule(spec):
            return None, (
                f"{name}: a cross-currency swap between two coupon dates is not "
                "simulated yet (the engine counts whole years to its maturity). It "
                "is left out of this netting set."
            )
        return (
            xva._Spec(
                name,
                "cross_currency_swap",
                spec.payer,
                float(len(left)),
                spec.notional,
                quantity=quantity,
                rate=spec.fixed_rate,
                foreign_rate=spec.foreign_rate,
                domestic_notional=spec.domestic_notional,
            ),
            None,
        )
    # A swap. Not started, or starting today: as it is.
    if d <= spec.start:
        return (
            xva._Spec(
                name,
                "swap",
                spec.payer,
                float(len(left)),
                spec.notional,
                start=t(d, spec.start),
                quantity=quantity,
                currency=spec.currency,
                rate=spec.fixed_rate,
            ),
            None,
        )
    # Started: the engine has no fixing of the past, so the coupon in
    # progress — a known amount — is left out and the rest starts on the
    # next coupon date.
    if len(left) == 1:
        return None, (
            f"{name}: only the coupon in progress is left, a known amount paid on "
            f"{left[0]}. It is not simulated."
        )
    return (
        xva._Spec(
            name,
            "swap",
            spec.payer,
            float(len(left) - 1),
            spec.notional,
            start=t(d, left[0]),
            quantity=quantity,
            currency=spec.currency,
            rate=spec.fixed_rate,
        ),
        f"{name}: the coupon in progress, a known amount paid on {left[0]}, is left "
        "out of the exposure; the coupons after it are simulated.",
    )


def trades(
    pf: Portfolio, netting_set_id: str, d: date
) -> Tuple[List[xva._Spec], List[str]]:
    """The trades held on `d` under a netting set, as the engine takes them,
    and what had to be left out."""
    held = portfolio_ledger.holdings(pf, d)
    specs: List[xva._Spec] = []
    said: List[str] = []
    for inst in pf.instruments:
        if inst.netting_set_id != netting_set_id:
            continue
        h = held.get(inst.id)
        if h is None or abs(h.quantity) < portfolio_ledger.QTY_EPS:
            continue
        if isinstance(inst.spec, DerivativeSpec):
            said.append(
                f"{inst.label or inst.id}: an equity derivative. The exposure engine "
                "simulates interest rates and the exchange rate, not yet a share "
                "price: it is left out of this netting set."
            )
            continue
        spec, note = _rate_fx(inst, inst.spec, h.quantity, d)
        if spec is not None:
            specs.append(spec)
        if note:
            said.append(note)
    return specs, said


def request(req: PortfolioXvaRequest) -> XvaRequest:
    """The request of the xVA page this netting set comes to: the
    counterparty's rating, sector and recovery, its collateral agreement."""
    found = portfolio_counterparties.netting_set(req.portfolio, req.netting_set_id)
    if found is None:
        raise xva.XvaInputError(
            f"The portfolio has no netting set {req.netting_set_id!r}."
        )
    party, netting_set = found
    return XvaRequest(
        portfolio="ledger",
        counterparty_rating=party.rating,
        own_rating=req.own_rating,
        recovery=party.recovery,
        csa=netting_set.csa,
        borrowing_spread=req.borrowing_spread,
        lending_spread=req.lending_spread,
        capital=CapitalInput(cost_of_capital=req.cost_of_capital, sector=party.sector),
        wrong_way_risk=req.wrong_way_risk,
        paths=req.paths,
        seed=req.seed,
        pfe_confidence=req.pfe_confidence,
        device=req.device,
    )


def compute(
    req: PortfolioXvaRequest, today: Optional[date] = None
) -> Tuple[XvaRequest, XvaResponse]:
    """Exposure and xVA of the netting set on today's market, and the
    request of the xVA page it was run as (for the audit record)."""
    try:
        pf = portfolio_ledger.migrate(req.portfolio)
        portfolio_ledger.validate(pf)
    except ValueError as exc:
        raise xva.XvaInputError(str(exc)) from exc
    run = request(req)
    party, netting_set = portfolio_counterparties.netting_set(pf, req.netting_set_id)

    def book(as_of: date):
        specs, said = trades(pf, req.netting_set_id, as_of)
        return (
            f"{party.name}: {netting_set.name}",
            "The trades of this portfolio under this netting set, with the life "
            "they have left on the date of the swap curve and the rates of their "
            "contracts.",
            specs,
            said,
        )

    return run, xva.compute(run, today, book)


__all__ = ["PortfolioXvaRequest", "compute", "request", "trades"]
