"""The share of a netting set that holds equity options — what the exposure
engine needs of it beside the interest rate (blueprint/wp/23-xva.md, lot X14).

The engine simulates the share as it simulates a second currency: its price
is the "exchange rate", its dividend yield the "foreign rate", known and
constant (models/rates/cross_currency_hull_white.hpp, make_asset_model). So
the share is lognormal under a Hull-White interest rate, and correlated with
it. Everything is read from the store, with no fallback:

- the close and the dividend yield of the share (a missing yield is zero,
  and said);
- its volatility: the implied volatility of the day's smile at the strike
  and expiry of the option the model is calibrated on, or, when no option
  chain is stored for it, its realised volatility over 63 business days — a
  proxy, shown as such (the Portfolio page's own rule, portfolio_models.py);
- its correlation with the interest rate: a historical estimate on weekly
  changes of the five-year Treasury yield and weekly log returns of the
  share, with its confidence interval. No traded price implies it.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Optional

import pandas as pd

from . import db, portfolio_models, portfolio_valuation, xva_fx
from .portfolio_schemas import DerivativeSpec

#: A close older than this is not the price of the day.
STALE_AFTER_DAYS = 7


class EquityUnavailable(RuntimeError):
    """An input the share's model needs is not in the store."""


@dataclass(frozen=True)
class AssetMarket:
    ticker: str
    #: An index (or a fund on one), for the supervisory factor of SA-CCR.
    index: bool
    spot: float
    spot_as_of: date
    dividend_yield: float
    dividend_as_of: Optional[date]
    #: The Black volatility the model is calibrated to, and of which option.
    market_volatility: float
    volatility_source: str
    volatility_proxied: bool
    calibration_strike: float
    calibration_expiry: float
    correlation: xva_fx.Estimate
    correlation_start: date
    correlation_end: date


def _correlation(ticker: str, as_of: date):
    since = as_of - timedelta(days=365 * xva_fx.CORRELATION_YEARS)
    rate = db.fred_series(xva_fx.USD_RATE_SERIES, since)
    closes = db.price_history(ticker, since)
    if rate.empty or not closes:
        raise EquityUnavailable(
            f"No history of {xva_fx.USD_RATE_SERIES} or of {ticker} in the store to "
            "estimate their correlation on"
        )
    share = pd.Series([float(c) for _, c in closes], index=[d for d, _ in closes])
    share = share[[d <= as_of for d in share.index]]
    # The rate moves by differences, the share by log returns.
    d_rate = (
        xva_fx._weekly(rate[rate.index <= pd.Timestamp(as_of)]).diff().dropna()
    )  # noqa: SLF001
    d_share = xva_fx._weekly(share).map(math.log).diff().dropna()  # noqa: SLF001
    try:
        estimate = xva_fx._correlation(d_rate, d_share)  # noqa: SLF001
    except xva_fx.TwoCurrencyUnavailable as exc:
        raise EquityUnavailable(f"{ticker} and the interest rate: {exc}") from exc
    common = pd.concat([d_rate, d_share], axis=1, join="inner").dropna()
    return estimate, common.index[0].date(), common.index[-1].date()


def market(option: DerivativeSpec, as_of: date) -> AssetMarket:
    """The share under `option`, calibrated on that option, on `as_of`."""
    ticker = option.underlying
    if not ticker:
        raise EquityUnavailable("an option without a listed underlying")
    md = portfolio_valuation.MarketData(as_of)
    got = md.closes(ticker).at(as_of)
    if got is None or (as_of - got[0]).days > STALE_AFTER_DAYS:
        raise EquityUnavailable(
            f"No close of {ticker} within {STALE_AFTER_DAYS} days of {as_of} in the store"
        )
    spot_as_of, spot = got[0], float(got[1])
    dividend = md.dividends(ticker).at(as_of)
    strike = float(option.params["strike"])
    expiry = (option.expiry - as_of).days / portfolio_valuation.YEAR_DAYS
    vol, param, _, smile = portfolio_models._asset_vol(  # noqa: SLF001
        option, dict(option.params), as_of, md, strike, expiry
    )
    if param.status == "default":
        raise EquityUnavailable(
            f"No option chain and too short a price history for {ticker}: no "
            "volatility to simulate it with"
        )
    estimate, start, end = _correlation(ticker, as_of)
    return AssetMarket(
        ticker=ticker,
        index=ticker.startswith("^"),
        spot=spot,
        spot_as_of=spot_as_of,
        dividend_yield=float(dividend[1]) if dividend else 0.0,
        dividend_as_of=dividend[0] if dividend else None,
        market_volatility=float(vol),
        volatility_source=param.source,
        volatility_proxied=smile is None,
        calibration_strike=strike,
        calibration_expiry=expiry,
        correlation=estimate,
        correlation_start=start,
        correlation_end=end,
    )


__all__ = ["AssetMarket", "EquityUnavailable", "market"]
