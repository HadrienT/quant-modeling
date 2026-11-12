"""Initial margin model, default probabilities and regulatory capital inputs."""

from __future__ import annotations

from typing import TYPE_CHECKING, List, Optional, Tuple


from .. import db

if TYPE_CHECKING:  # pragma: no cover — xva_equity imports the portfolio modules
    pass
from ..xva_schemas import (
    CapitalOut,
    CreditInput,
    HistoricalDefaultRate,
    XvaRequest,
)
from .base import XvaInputError, XvaUnavailable
from .demos import _Spec
from .trades import _has_asset, _two_currencies


def _margin_model(req: XvaRequest, specs: List[_Spec]) -> Tuple[str, str]:
    """The model of the initial margin, and why. SIMM needs the sensitivities
    of every trade in every scenario, which the closed forms give for swaps
    and European swaptions; a Bermudan is valued by regression and has none."""
    asked = req.csa.initial_margin_model
    by_regression = sorted({s.kind for s in specs} - {"swap", "swaption", "cashflow"})
    if req.csa.initial_margin_today is not None:
        if asked == "simm":
            raise XvaInputError(
                "An initial margin given for today goes with the regression model, "
                "which it scales; SIMM computes today's margin itself."
            )
        return "regression", (
            "The margin you gave for today is the starting point: the regression "
            "model gives the shape of the profile, scaled to start from it."
        )
    if asked == "regression":
        return "regression", "Chosen by hand."
    if _has_asset(specs):
        if asked == "simm":
            raise XvaInputError(
                "SIMM is computed here for the interest rate risk of one currency. "
                "This netting set also moves with a share: use the regression model."
            )
        return "regression", (
            "This netting set holds equity options. The SIMM computed here covers "
            "interest rate risk, not equity risk, so the margin is modelled instead: "
            "the 99 % quantile of the move of the value over ten days, fitted on the "
            "simulated paths."
        )
    if _two_currencies(specs):
        if asked == "simm":
            raise XvaInputError(
                "SIMM is computed here for the interest rate risk of one currency. "
                "This netting set also moves with the exchange rate and with euro "
                "rates: use the regression model."
            )
        return "regression", (
            "This netting set has two currencies. The SIMM computed here covers the "
            "interest rate risk of one currency, not the exchange rate nor a second "
            "curve, so the margin is modelled instead: the 99 % quantile of the move "
            "of the value over ten days, fitted on the simulated paths."
        )
    if by_regression:
        if asked == "simm":
            raise XvaInputError(
                "SIMM on every path needs the sensitivities of each trade in each "
                f"scenario; a {by_regression[0]} is valued by regression and has none. "
                "Use the regression model for this portfolio."
            )
        return "regression", (
            f"This portfolio holds a {by_regression[0]}, valued by regression: it has "
            "no sensitivity to each point of the curve in each scenario, which SIMM "
            "needs. The margin is modelled instead: the 99 % quantile of the move of "
            "the value over ten days, fitted on the simulated paths."
        )
    return "simm", (
        "Chosen by hand."
        if asked == "simm"
        else "Every trade is a swap or a European swaption, whose sensitivities are "
        "known in closed form in each scenario: the margin is ISDA SIMM itself, the "
        "rule the industry uses, rather than a model of it."
    )


#: The PD floor of the IRB formula (Basel framework, CRE32.4).
_PD_FLOOR = 0.0005
_INVESTMENT_GRADE = {"AAA", "AA", "A", "BBB"}


#: Whose default statistics the capital's PD is read from: the agency whose
#: scale the ratings of this page are written in.
_DEFAULT_RATE_AGENCY = "S&P"
#: Fewer years than this is not a long-run average.
_DEFAULT_RATE_MIN_YEARS = 10


def _default_rate(rating: str) -> HistoricalDefaultRate:
    """The historical one-year default rate of a rating: the average of the
    yearly rates of its category. A capital formula wants this frequency, not
    the intensity a spread implies under the pricing measure, which also pays
    for bearing the risk."""
    rows = db.rating_default_rates(_DEFAULT_RATE_AGENCY, rating)
    if len(rows) < _DEFAULT_RATE_MIN_YEARS:
        raise XvaUnavailable(
            f"The historical default rates of rating {rating} are not in the store "
            "(run data-ingest's rating-default-rates source with --full), and no "
            "probability of default was given for the capital"
        )
    return HistoricalDefaultRate(
        agency=_DEFAULT_RATE_AGENCY,
        rating=rating,
        first_year=rows[0][0].year,
        last_year=rows[-1][0].year,
        years=len(rows),
        defaults=sum(n for _, n, _ in rows),
        rate=sum(r for _, _, r in rows) / len(rows) / 100.0,
    )


def _capital_method(req: XvaRequest, specs: List[_Spec]) -> Tuple[str, str]:
    """How the exposure at default is measured, and why. The standardised
    approach works from a supervisory description of each trade (asset class,
    notional, dates, side), which a script does not carry; the internal models
    method reads the exposure off the simulation, whatever the trade."""
    asked = req.capital.method
    scripted = any(s.kind == "script" for s in specs)
    if asked == "sa_ccr" and scripted:
        raise XvaInputError(
            "The standardised approach (SA-CCR) needs a supervisory description of "
            "each trade — asset class, notional, dates, side — which a script does "
            "not carry. Use the internal models method for a netting set that "
            "holds a script."
        )
    if asked != "auto":
        return asked, "Chosen by hand."
    if scripted:
        return "internal_model", (
            "This netting set holds a trade written as a script, which has no "
            "supervisory description for the standardised approach. Its exposure "
            "at default is read off the simulation instead, as the internal "
            "models method does: 1.4 × the Effective EPE."
        )
    if _has_asset(specs):
        return "sa_ccr", (
            "Every trade has a supervisory description for the standardised "
            "approach (SA-CCR), the method a bank uses without an approved model: "
            "the rate trades form the dollar hedging set, and an equity option is "
            "an equity trade, by the price of the shares it is on."
        )
    if _two_currencies(specs):
        return "sa_ccr", (
            "Every trade has a supervisory description for the standardised "
            "approach (SA-CCR), the method a bank uses without an approved model: "
            "each currency's rate trades form a hedging set, and the forward and "
            "the cross-currency swap are foreign-exchange trades, by the notional "
            "of their euro leg converted into dollars."
        )
    return "sa_ccr", (
        "Every trade is a swap or a swaption, which the standardised approach "
        "(SA-CCR) describes: the method a bank uses without an approved model."
    )


def _capital_inputs(
    req: XvaRequest,
    counterparty: CreditInput,
    default_rate: Optional[HistoricalDefaultRate],
    method: str,
) -> dict:
    """What the capital projection takes: the counterparty's regulatory
    parameters and the cost of capital."""
    c = req.capital
    pd = c.pd if c.pd is not None else default_rate.rate
    return {
        "method": method,
        "pd": pd,
        # Foundation approach, CRE32.6.
        "lgd": (
            c.lgd if c.lgd is not None else (0.45 if c.sector == "financial" else 0.40)
        ),
        "sector": c.sector,
        "investment_grade": counterparty.rating in _INVESTMENT_GRADE,
        "cost_of_capital": c.cost_of_capital,
    }


def _pd_reason(inputs: dict, default_rate: Optional[HistoricalDefaultRate]) -> str:
    floored = (
        f" The regulatory floor of {100.0 * _PD_FLOOR:.2f} % applies."
        if inputs["pd"] < _PD_FLOOR
        else ""
    )
    if default_rate is None:
        return "The probability of default given in the request." + floored
    d = default_rate
    return (
        f"Average one-year default rate of {d.agency}'s {d.rating} corporate "
        f"ratings from {d.first_year} to {d.last_year}: {100.0 * d.rate:.2f} % "
        f"({d.defaults} default{'' if d.defaults == 1 else 's'} in {d.years} "
        "years, as the agency reported them to ESMA)." + floored
    )


def _capital(
    r: dict,
    inputs: dict,
    default_rate: Optional[HistoricalDefaultRate],
    margined: bool,
    method_reason: str,
) -> CapitalOut:
    k = r["capital"]
    return CapitalOut(
        method=inputs["method"],
        method_reason=method_reason,
        ead_today=k["ead_today"],
        default_capital_today=k["default_capital_today"],
        cva_capital_today=k["cva_capital_today"],
        times=k["times"],
        expected_ead=k["expected_ead"],
        discounted_capital=k["discounted_capital"],
        pd=max(inputs["pd"], _PD_FLOOR),
        pd_source="entered" if default_rate is None else "historical",
        pd_reason=_pd_reason(inputs, default_rate),
        lgd=inputs["lgd"],
        sector=inputs["sector"],
        investment_grade=inputs["investment_grade"],
        margined=margined,
        cost_of_capital=inputs["cost_of_capital"],
    )
