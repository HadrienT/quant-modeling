"""Constants and errors of the xVA package."""

from __future__ import annotations

from datetime import date
from typing import TYPE_CHECKING

if TYPE_CHECKING:  # pragma: no cover — xva_equity imports the portfolio modules
    pass


#: Notional of the reference trade of every portfolio, USD.
NOTIONAL = 10_000_000.0
#: Notional of the trades in the second currency, EUR: about the same size.
FOREIGN_NOTIONAL = 9_000_000.0
#: The trades that exist only with two currencies.
_FX_KINDS = ("fx_forward", "cross_currency_swap")
#: The series the historical dynamics are estimated on, and from when.
HISTORY_SERIES = "DGS3MO"
HISTORY_SINCE = date(1990, 1, 1)
_BUSINESS_DAYS = 250.0


class XvaUnavailable(RuntimeError):
    """An input the computation needs is not in the store."""


class XvaInputError(ValueError):
    """A request the computation cannot honour as asked."""
