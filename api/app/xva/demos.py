"""The demonstration netting sets and the ratings the credit curves are proxied by."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Dict, List, Optional, Tuple


from .. import credit

if TYPE_CHECKING:  # pragma: no cover — xva_equity imports the portfolio modules
    pass
from .base import FOREIGN_NOTIONAL, NOTIONAL


@dataclass(frozen=True)
class _Spec:
    description: str
    #: "swap" | "swaption" | "bermudan" | "script" | "fx_forward" |
    #: "cross_currency_swap" | "equity_option" | "cashflow" (an amount
    #: already fixed: `notional` paid at `tenor`)
    kind: str
    #: A payer swap; for an FX trade, the side that receives the euros; a
    #: call, for an equity option.
    payer: bool
    #: The tenor of a swap; the maturity of an FX trade; the expiry of an
    #: equity option.
    tenor: float
    #: In the trade's currency; in euros for an FX trade; the number of
    #: shares an equity option is on.
    notional: float
    #: The expiry of a swaption; the first exercise date of a Bermudan.
    start: float = 0.0
    quantity: float = 1.0
    #: (start, tenor) of the swap whose par rate is the fixed rate, when it is
    #: not the trade's own swap: the right to cancel a swap is struck at that
    #: swap's rate.
    par_of: Optional[Tuple[float, float]] = None
    #: The text of a scripted trade; None for the swap this module writes as
    #: a script itself, at today's par rate.
    script: Optional[str] = None
    #: The currency of a rate trade.
    currency: str = "USD"
    #: The terms of a trade that is not struck at today's market (a trade of
    #: a portfolio, portfolio_xva.py); None for the market's. `rate` is the
    #: fixed rate of a swap, the strike of a swaption, the delivery rate of
    #: an FX forward, the dollar coupon of a cross-currency swap.
    rate: Optional[float] = None
    foreign_rate: Optional[float] = None
    domestic_notional: Optional[float] = None
    #: For an amount already fixed: (notional, end) of the swap it is the
    #: coupon in progress of — its supervisory description.
    swap: Optional[Tuple[float, float]] = None


def _swap(tenor: float, payer: bool, notional: float = NOTIONAL) -> _Spec:
    side = "Payer" if payer else "Receiver"
    return _Spec(f"{side} swap {tenor:g}Y at par", "swap", payer, tenor, notional)


def _euro_swap(tenor: float, payer: bool, notional: float = FOREIGN_NOTIONAL) -> _Spec:
    side = "Payer" if payer else "Receiver"
    return _Spec(
        f"{side} swap {tenor:g}Y in euros, at par",
        "swap",
        payer,
        tenor,
        notional,
        currency="EUR",
    )


def _fx_forward(
    maturity: float, buys_euros: bool, notional: float = FOREIGN_NOTIONAL
) -> _Spec:
    side = "Buys" if buys_euros else "Sells"
    return _Spec(
        f"{side} EUR {notional / 1e6:g} million against dollars in {maturity:g}Y, "
        "at the forward rate",
        "fx_forward",
        buys_euros,
        maturity,
        notional,
    )


def _cross_currency_swap(
    maturity: float, receives_euros: bool, notional: float = FOREIGN_NOTIONAL
) -> _Spec:
    legs = (
        "receives euros, pays dollars"
        if receives_euros
        else "pays euros, receives dollars"
    )
    return _Spec(
        f"Cross-currency swap {maturity:g}Y, fixed for fixed: {legs}; EUR "
        f"{notional / 1e6:g} million against their value in dollars today, "
        "exchanged back at maturity",
        "cross_currency_swap",
        receives_euros,
        maturity,
        notional,
    )


def _swaption(
    expiry: float, tenor: float, bought: bool, notional: float = NOTIONAL
) -> _Spec:
    side = "Bought" if bought else "Sold"
    return _Spec(
        f"{side} payer swaption {expiry:g}Y into {tenor:g}Y, at the money",
        "swaption",
        True,
        tenor,
        notional,
        start=expiry,
        quantity=1.0 if bought else -1.0,
    )


def _scripted_swap(tenor: int, notional: float = NOTIONAL) -> _Spec:
    return _Spec(
        f"Payer swap {tenor:g}Y at par, written as a script",
        "script",
        True,
        float(tenor),
        notional,
    )


def _bermudan(
    first: float,
    tenor: float,
    payer: bool,
    notional: float = NOTIONAL,
    par_of: Optional[Tuple[float, float]] = None,
) -> _Spec:
    side = "payer" if payer else "receiver"
    return _Spec(
        f"Bought Bermudan {side} swaption, exercisable each year from {first:g}Y "
        f"into what is left of a swap ending at {first + tenor:g}Y",
        "bermudan",
        payer,
        tenor,
        notional,
        start=first,
        par_of=par_of,
    )


#: id -> (label, what it teaches, trades).
PORTFOLIOS: Dict[str, Tuple[str, str, List[_Spec]]] = {
    "single_swap": (
        "One 10-year payer swap",
        "The exposure of a swap is a hump: rates diffuse like the square root of "
        "time while the remaining coupons run off. CVA is close to the credit "
        "spread times the average exposure times the maturity.",
        [_swap(10, True)],
    ),
    "bought_swaption": (
        "One bought swaption",
        "An option that was paid for can only be an asset: until it expires its "
        "discounted expected exposure is its price, and the CVA of that period "
        "is the loss given default times the price times the probability of "
        "default. Once exercised it is a swap.",
        [_swaption(1, 10, True)],
    ),
    "sold_swaption": (
        "One sold swaption",
        "The seller received the premium and can only owe: no exposure and no "
        "CVA until the expiry, only a DVA. After a physical exercise it is a "
        "swap, which can turn either way.",
        [_swaption(1, 10, False)],
    ),
    "directional": (
        "A directional book: four payer swaps",
        "Trades that move together do not net: the exposure of the set is "
        "almost the sum of the exposures of its trades, and every trade adds "
        "to the CVA.",
        [_swap(2, True), _swap(5, True), _swap(10, True), _swap(20, True)],
    ),
    "balanced": (
        "A balanced book: payers, receivers and an option",
        "Close-out netting: trades that offset each other reduce the exposure "
        "of the set well below the sum of its trades. A trade can have a "
        "positive incremental CVA: it lowers the counterparty risk of what is "
        "already there.",
        [
            _swap(10, True),
            _swap(10, False, 0.8 * NOTIONAL),
            _swap(5, True, 0.6 * NOTIONAL),
            _swap(7, False, 0.6 * NOTIONAL),
            _swap(2, False),
            _swaption(2, 10, True, 0.5 * NOTIONAL),
        ],
    ),
    "bermudan": (
        "One bought Bermudan swaption",
        "A Bermudan can be exercised on several dates, so no formula gives what "
        "it will be worth: its future value is estimated by regression on the "
        "simulated rates (American Monte-Carlo). Until it is exercised it is an "
        "asset, like any bought option. Each path that exercises becomes a swap, "
        "which can turn either way.",
        [_bermudan(1, 9, True)],
    ),
    "cancellable": (
        "A cancellable swap",
        "A 10-year payer swap with the right to cancel it on each anniversary: "
        "that right is a Bermudan receiver swaption on what is left of the swap. "
        "The bank cancels when the swap has turned against it, so the right "
        "removes what the bank would owe, not what it is owed: the DVA shrinks, "
        "and the option, an asset, adds its own exposure.",
        [_swap(10, True), _bermudan(1, 9, False, par_of=(0.0, 10.0))],
    ),
    "scripted_swap": (
        "A swap written as a script",
        "The 10-year payer swap again, this time written in the payoff language "
        "of the Scripting page. Nothing tells the engine that it is a swap: it "
        "replays the script on every scenario, reads what it pays, and estimates "
        "its value by regression. The exposure and the CVA are those of the "
        "native swap; the capital is read off the simulated exposure, because a "
        "script has no regulatory description of itself.",
        [_scripted_swap(10)],
    ),
    "fx_forward": (
        "One 5-year EUR/USD forward",
        "A second risk factor: the exchange rate. Nothing is paid before the "
        "delivery, so nothing amortises: the exposure grows like the square root "
        "of time until the last day, where a swap's has long since turned down. "
        "The two interest rates move the forward as well, and more as the "
        "delivery is far.",
        [_fx_forward(5, True)],
    ),
    "cross_currency": (
        "One 10-year cross-currency swap",
        "The notionals are exchanged back at maturity at the rate agreed today: "
        "the swap is, above all, a ten-year FX forward on its whole notional. "
        "Its exposure keeps rising until the end and is several times that of "
        "an interest rate swap of the same size — compare with the first "
        "portfolio.",
        [_cross_currency_swap(10, True)],
    ),
    "two_currencies": (
        "A book in two currencies",
        "Dollar swaps, a euro swap, a cross-currency swap and a forward, netted "
        "together in dollars: the exchange rate is in the value of every euro "
        "trade, and the trades on it weigh more in the CVA than their number "
        "suggests.",
        [
            _swap(10, True),
            _swap(5, False, 0.6 * NOTIONAL),
            _euro_swap(10, False),
            _cross_currency_swap(5, False),
            _fx_forward(2, True),
        ],
    ),
    "custom": (
        "Your own scripts",
        "The netting set is made of the scripts of the request alone: any payoff "
        "on interest rates the language can write has an exposure, a CVA and a "
        "cost of capital.",
        [],
    ),
}

_RATING_SERIES = {
    ("CCC" if r.label.startswith("CCC") else r.label): r.series_id
    for r in credit.RATINGS
}
RATINGS: List[str] = list(_RATING_SERIES)
