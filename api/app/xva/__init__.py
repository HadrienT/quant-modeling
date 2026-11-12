"""xVA of a netting set — exposure, collateral, CVA and DVA on market inputs
(blueprint/wp/23-xva.md, lot X3).

A teaching tool on real data (§13.7 of the work package): a handful of
portfolios, from one swap to a hedged book, each showing one idea of Gregory's
*The xVA Challenge*. Everything the computation rests on is read from the
store, with no fallback:

- the USD SOFR swap curve and the swaption vols Hull-White is calibrated on,
  both from traded prices (`rates_derivatives.market_request`);
- the credit curves of the counterparty and of the bank, proxied by rating
  from the ICE BofA option-adjusted spreads (as on the credit page);
- the historical dynamics of the short rate, estimated on the 3-month
  Treasury yield, for the risk measures — with the user free to override
  them, because the data pin down the volatility and little else.

The C++ does the work in one call (`qm.xva_netting_set`); this package builds
the portfolios, gathers the inputs and carries the methodology.

The package splits the module by role: `demos` (the demonstration
portfolios), `market` (the inputs read from the store), `trades` (the trades
as the library reads them), `capital` (initial margin and capital inputs),
`calculation` (the two library calls) and `methodology_text` (what the page
explains).
"""

from .base import (
    FOREIGN_NOTIONAL,
    HISTORY_SERIES,
    HISTORY_SINCE,
    NOTIONAL,
    XvaInputError,
    XvaUnavailable,
)
from .demos import _RATING_SERIES, PORTFOLIOS, RATINGS, _Spec
from .trades import _two_currencies, swap_script
from .calculation import Book, compute, compute_sensitivities, market_inputs
from .methodology_text import (
    methodology,
    portfolios,
    sensitivities_methodology,
    wrong_way_scenarios,
)

__all__ = [
    "Book",
    "FOREIGN_NOTIONAL",
    "HISTORY_SERIES",
    "HISTORY_SINCE",
    "NOTIONAL",
    "PORTFOLIOS",
    "RATINGS",
    "XvaInputError",
    "XvaUnavailable",
    "_RATING_SERIES",
    "_Spec",
    "_two_currencies",
    "compute",
    "compute_sensitivities",
    "market_inputs",
    "methodology",
    "portfolios",
    "sensitivities_methodology",
    "swap_script",
    "wrong_way_scenarios",
]
