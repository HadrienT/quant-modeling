"""
Local volatility pipeline — Dupire calibration from live option chains.

Stages
------
1. fetcher    — fetch raw option chain for a ticker (yfinance)             [Python]
2. cleaner    — liquidity filters + static-arbitrage removal               [Python]
3. iv_surface — bicubic spline implied-vol surface in (y, T) space        [Python]
4. dupire     — Gatheral (2004) formula → local vol surface + grid export [Python]
5. cpp_bridge — adapter that passes the grid to the C++ Euler-Maruyama MC [C++]
"""
from .cleaner import CleaningStats, clean_chain
from .dupire import LocalVolSurface, calibrate_dupire
from .fetcher import OptionQuote, fetch_option_chain
from .iv_surface import IVSurface, build_iv_surface
from .cpp_bridge import LocalVolPricingResult, price_with_local_vol

__all__ = [
    "OptionQuote",
    "fetch_option_chain",
    "CleaningStats",
    "clean_chain",
    "IVSurface",
    "build_iv_surface",
    "LocalVolSurface",
    "calibrate_dupire",
    "LocalVolPricingResult",
    "price_with_local_vol",
]
