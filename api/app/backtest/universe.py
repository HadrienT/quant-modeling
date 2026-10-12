"""What a backtest can be run on: the tickers of `prices.raw_daily`.

The exchange-traded funds come first, and are the page's default universe:
a fund that exists today existed all along, so a basket of them carries no
survivorship bias. The S&P 500 names do: Yahoo has no prices for a delisted
company, so only the members that survived have a history (186 of the 491
members of January 2000, data-ingest README).
"""

from __future__ import annotations

from .. import db
from ..cache import TTLCache
from . import engine, market
from . import schemas as s

#: The funds data-ingest stores bars for (`_equity_raw_common.ETFS`).
ETFS = {
    "SPY": "SPDR S&P 500",
    "QQQ": "Invesco QQQ (Nasdaq-100)",
    "IWM": "iShares Russell 2000",
    "VTI": "Vanguard Total Stock Market",
    "URTH": "iShares MSCI World",
    "EFA": "iShares MSCI EAFE",
    "EEM": "iShares MSCI Emerging Markets",
    "TLT": "iShares 20+ Year Treasury Bond",
    "IEF": "iShares 7-10 Year Treasury Bond",
    "LQD": "iShares Investment Grade Corporate Bond",
    "HYG": "iShares High Yield Corporate Bond",
    "GLD": "SPDR Gold Shares",
    "XLB": "Materials Select Sector SPDR",
    "XLC": "Communication Services Select Sector SPDR",
    "XLE": "Energy Select Sector SPDR",
    "XLF": "Financial Select Sector SPDR",
    "XLI": "Industrial Select Sector SPDR",
    "XLK": "Technology Select Sector SPDR",
    "XLP": "Consumer Staples Select Sector SPDR",
    "XLRE": "Real Estate Select Sector SPDR",
    "XLU": "Utilities Select Sector SPDR",
    "XLV": "Health Care Select Sector SPDR",
    "XLY": "Consumer Discretionary Select Sector SPDR",
}

_cache: TTLCache[str, s.UniverseResponse] = TTLCache(max_size=1, ttl_seconds=3600)


def universe() -> s.UniverseResponse:
    cached = _cache.get("universe")
    if cached is not None:
        return cached
    names = {
        m.ticker: m.name or m.ticker
        for m in db.market_members("SP500")
        if m.kind == "equity"
    }
    coverage = db.raw_daily_coverage([*ETFS, *names])

    def assets(catalogue: dict[str, str]) -> list[s.Asset]:
        return [
            s.Asset(ticker=t, name=name, first=coverage[t][0], last=coverage[t][1])
            for t, name in catalogue.items()
            if t in coverage
        ]

    etfs = assets(ETFS)
    if not etfs:
        raise market.MissingData("prices.raw_daily holds no exchange-traded fund.")
    response = s.UniverseResponse(
        etfs=etfs,
        stocks=assets(dict(sorted(names.items()))),
        benchmark=market.BENCHMARK,
        cash_series=market.CASH_SERIES,
        first=min(a.first for a in etfs),
        last=max(a.last for a in etfs),
        engine=engine.build(),
    )
    _cache.set("universe", response)
    return response
