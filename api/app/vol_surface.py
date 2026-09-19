"""
Vol surface pipeline: option chain -> SVI per maturity -> Dupire local vol.

Replaces api/app/local_vol/{fetcher,cleaner,iv_surface,dupire,cpp_bridge}.py
and the old routers/market_iv_surface.py's independent yfinance/griddata
path (since removed in favour of raw_iv_grid below) -- those had drifted
into two unrelated implementations of the same problem. There is one fetch,
and one C++ pipeline (market/vol_surface_pipeline.hpp, bound as
quantmodeling.calibrate_vol_surface) for everything past it: cleaning, SVI
calibration per maturity, and the Dupire local-vol grid.

Fetching stays in Python and has one source: data-ingest's Postgres
(options.chain_snapshot, prices.sp500_daily, prices.dividend_yields), which
covers the small fixed ticker universe options-chain-snapshot tracks (see
~/data-ingest's README). There is no live fallback: a ticker or a value the
database does not hold is reported as missing (NoOptionChainAvailable, or
db.StoreUnavailable if the database itself is unreachable).
"""

from __future__ import annotations

import logging
import math
from datetime import date
from typing import List, Optional

import quantmodeling as qm

from . import db

logger = logging.getLogger(__name__)

MIN_CLEAN_QUOTES = 16  # below this, a fit is not meaningfully constrained


class NoOptionChainAvailable(RuntimeError):
    """The database holds no (recent enough) market data for this ticker."""


def _ttm(expiry: date, as_of: date) -> float:
    """Calendar-day TTM as a fraction of 365, floored at 0 for an expired quote."""
    return max((expiry - as_of).days, 0) / 365.0


def _quote(
    strike: float,
    ttm: float,
    is_call: bool,
    bid: float,
    ask: float,
    last: float,
    volume: int,
    open_interest: int,
    implied_vol: Optional[float],
) -> "qm.RawOptionQuote":
    q = qm.RawOptionQuote()
    q.strike = strike
    q.ttm = ttm
    q.is_call = is_call
    q.bid = bid
    q.ask = ask
    q.last = last
    q.volume = volume
    q.open_interest = open_interest
    has_iv = (
        implied_vol is not None and implied_vol == implied_vol and implied_vol > 0
    )  # NaN-safe
    q.implied_vol = float(implied_vol) if has_iv else 0.0
    q.has_iv = has_iv
    return q


def fetch_option_chain(ticker: str) -> List["qm.RawOptionQuote"]:
    """The most recent option-chain snapshot data-ingest has stored for this
    ticker. Raises NoOptionChainAvailable if it is not in the tracked
    universe (OPTIONS_CHAIN_TICKERS), db.StoreUnavailable if the database is
    unreachable -- never a live fetch."""
    ticker = ticker.upper().strip()
    rows = db.options_chain_snapshot(ticker)

    quotes: List[qm.RawOptionQuote] = []
    for r in rows:
        ttm = _ttm(r.expiry, r.snapshot_date)
        if ttm <= 0:
            continue
        quotes.append(
            _quote(
                strike=r.strike,
                ttm=ttm,
                is_call=(r.option_type == "call"),
                bid=r.bid or 0.0,
                ask=r.ask or 0.0,
                last=r.last_price or 0.0,
                volume=r.volume,
                open_interest=r.open_interest,
                implied_vol=r.implied_volatility,
            )
        )
    if not quotes:
        raise NoOptionChainAvailable(
            f"No stored option chain for '{ticker}': data-ingest's "
            "options-chain-snapshot tracks a fixed universe "
            "(OPTIONS_CHAIN_TICKERS), and this ticker is not in it."
        )
    logger.info("vol_surface: stored snapshot for %s (%d quotes)", ticker, len(quotes))
    return quotes


def get_spot(ticker: str) -> float:
    """Latest close from prices.sp500_daily. Raises NoOptionChainAvailable if
    there is none recent enough (db.latest_price's staleness rule) -- never a
    live price."""
    ticker = ticker.upper().strip()
    cached = db.latest_price(ticker)
    if cached is None:
        raise NoOptionChainAvailable(
            f"No recent close for '{ticker}' in prices.sp500_daily "
            "(missing, or older than the staleness limit). Refresh it with "
            "`ingest run sp500-prices`."
        )
    as_of, price = cached
    logger.info("vol_surface: spot for %s from DB (%s): %.2f", ticker, as_of, price)
    return price


def get_dividend_yield(ticker: str) -> float:
    """Latest trailing yield from prices.dividend_yields. Raises
    NoOptionChainAvailable if there is none recent enough -- never a live
    value, and never a silent 0.0 (a non-payer is stored as 0.0, so a missing
    row means "never ingested")."""
    ticker = ticker.upper().strip()
    cached = db.latest_dividend_yield(ticker)
    if cached is None:
        raise NoOptionChainAvailable(
            f"No recent dividend yield for '{ticker}' in prices.dividend_yields "
            "(dividend-yields covers the options-chain default universe only)."
        )
    as_of, value = cached
    logger.info(
        "vol_surface: dividend yield for %s from DB (%s): %.4f", ticker, as_of, value
    )
    return value


# ---------------------------------------------------------------------------
# SVI evaluation on the Python side, for the "cleaned" IV-surface display --
# the calibration result carries per-slice (a, b, rho, m, sigma), and this
# is the same linear-in-T interpolation SVISurface does in C++
# (market/svi_surface.hpp), just for display rather than for feeding Dupire.
# ---------------------------------------------------------------------------


def _svi_total_variance(
    k: float, a: float, b: float, rho: float, m: float, sigma: float
) -> float:
    x = k - m
    return a + b * (rho * x + math.sqrt(x * x + sigma * sigma))


def raw_iv_grid(
    quotes: List["qm.RawOptionQuote"],
) -> tuple[List[float], List[float], List[List[Optional[float]]]]:
    """The actual listed (strike, maturity) grid, no interpolation, no
    fitting: holes are strike/maturity pairs nothing traded at, not an
    artifact of a regular mesh laid over scattered points. Call and put IV
    at the same (strike, maturity) are averaged when both exist -- under
    put-call parity they should agree; when they don't, that disagreement is
    itself a liquidity signal, not something to hide by picking one side.

    Deliberately does not run RawVolSurface's cleaning stages (unlike the
    SVI-calibration path): this is the "brute" point on the raw -> cleaned ->
    local-vol progression, so a viewer can see what cleaning actually
    removes. The 3D surface view's percentile clipping (SurfaceGrid.ts)
    keeps a handful of implausible quotes from dominating the display.
    """
    buckets: dict[tuple[float, float], List[float]] = {}
    for q in quotes:
        if not q.has_iv:
            continue
        key = (round(q.ttm, 6), round(q.strike, 2))
        buckets.setdefault(key, []).append(q.implied_vol)

    maturities = sorted({k[0] for k in buckets})
    strikes = sorted({k[1] for k in buckets})

    values: List[List[Optional[float]]] = []
    for t in maturities:
        row: List[Optional[float]] = []
        for k in strikes:
            ivs = buckets.get((t, k))
            row.append(round(sum(ivs) / len(ivs), 6) if ivs else None)
        values.append(row)

    return strikes, maturities, values


def nearest_slice(slices: List[dict], ttm: float) -> dict:
    """The calibrated SVI slice whose own maturity is closest to `ttm` --
    same rationale as svi_implied_vol_grid's T-clamping: never invent a
    maturity the chain didn't actually have quotes for."""
    return min(slices, key=lambda s: abs(s["ttm"] - ttm))


def atm_vol_from_svi_slice(slc: dict, ttm: Optional[float] = None) -> float:
    """ATM implied vol from a calibrated SVI slice: total variance at
    k=0 (a + b*sigma at rho=0, but a + b*(rho*(-m) + sqrt(m^2+sigma^2)) in
    general -- see market/svi.hpp), divided by the slice's own ttm, unless a
    different ttm is supplied to reuse the same total-variance level (SVI
    parameters are only meaningful in-slice, not across maturities)."""
    w_atm = _svi_total_variance(0.0, slc["a"], slc["b"], slc["rho"], slc["m"], slc["sigma"])
    t = ttm if ttm is not None else slc["ttm"]
    return math.sqrt(max(w_atm, 0.0) / t)


def svi_iv_at_k(k: float, slc: dict) -> float:
    """SVI-implied vol at an arbitrary log-moneyness k -- atm_vol_from_svi_slice
    generalised to any k, used by the delta-bucket machinery below."""
    w = _svi_total_variance(k, slc["a"], slc["b"], slc["rho"], slc["m"], slc["sigma"])
    return math.sqrt(max(w, 0.0) / slc["ttm"])


def _norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def _bs_forward_delta(k: float, ttm: float, vol: float, is_call: bool) -> float:
    """Black-76 forward delta (no discounting -- consistent with this
    project's forward-based convention, e.g. models/equity/sabr.hpp's
    black76_* functions): d1 = (ln(F/K) + 0.5*vol^2*T) / (vol*sqrt(T)), and
    ln(F/K) = -k since k := ln(K/F). Call delta = N(d1), put delta =
    N(d1) - 1 (both in [-1, 1], the standard desk quoting convention for
    delta buckets -- see routers/local_vol_pricing.py's delta_surface)."""
    if vol <= 0.0 or ttm <= 0.0:
        return 0.0
    d1 = (-k + 0.5 * vol * vol * ttm) / (vol * math.sqrt(ttm))
    nd1 = _norm_cdf(d1)
    return nd1 if is_call else nd1 - 1.0


def solve_k_for_delta(
    slc: dict, target_delta: float, is_call: bool, k_min: float, k_max: float
) -> Optional[float]:
    """The log-moneyness k whose Black-76 forward delta (computed
    self-consistently against the SVI-implied vol AT that k, not a single
    fixed vol) equals target_delta, by bisection on [k_min, k_max].

    Delta is monotonically decreasing in k for both calls and puts (higher
    strike -> lower delta) SO LONG AS SVI's total variance stays close to
    its calibrated region: far enough into either wing, w(k) is
    asymptotically linear in k (sqrt((k-m)^2+sigma^2) -> |k-m|), so vol
    grows like sqrt(|k|) without bound -- and depending on the slice's own
    b/rho, that can make d1, and so delta, stop being monotonic altogether.
    Returns None rather than a wrong number when the target isn't bracketed
    (f_lo and f_hi need opposite signs) -- found by testing a real AAPL
    chain, where clamping to the nearest boundary produced "10-delta" vols
    above 300% for several maturities where the search range had drifted
    into that non-monotonic region. A missing delta bucket is honest; a
    clamped one that looks like a real number is not (see this project's
    standing rule that a displayed number must be defensible, not just
    computed).
    """

    def f(k: float) -> float:
        vol = svi_iv_at_k(k, slc)
        return _bs_forward_delta(k, slc["ttm"], vol, is_call) - target_delta

    lo, hi = k_min, k_max
    f_lo, f_hi = f(lo), f(hi)
    if f_lo < 0.0 or f_hi > 0.0:
        return None

    for _ in range(60):
        mid = 0.5 * (lo + hi)
        f_mid = f(mid)
        if abs(f_mid) < 1e-9:
            return mid
        if f_mid > 0.0:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def tenor_label(ttm: float) -> str:
    """Human tenor label from a year-fraction -- display only, a rounding
    heuristic rather than exact ISDA tenor conventions."""
    days = ttm * 365.0
    if days < 10:
        return f"{max(round(days), 1)}D"
    weeks = days / 7.0
    if weeks < 8:
        return f"{round(weeks)}W"
    months = days / 30.44
    if months < 23:
        return f"{round(months)}M"
    return f"{days / 365.0:.1f}Y"


def delta_bucket_row(slc: dict) -> dict:
    """One row of a desk-style delta-bucketed vol matrix: 10-delta put,
    25-delta put, ATM, 25-delta call, 10-delta call, plus the risk
    reversals and butterflies a desk actually quotes skew and convexity as
    (see routers/local_vol_pricing.py's delta_surface docstring) -- derived
    entirely from this one calibrated SVI slice, no new market data.

    Deliberately does NOT use the display grid's globally-clamped k_min/k_max
    (VolSurfacePipelineResult::k_min, intersected across every maturity so
    the rectangular K/T display grid stays inside what the shortest-dated,
    narrowest slice observed -- see calibrate_vol_surface). That clamp is
    exactly right for a shared grid, but wrong here: applied uniformly, a
    1-day slice's necessarily tiny k-range silently capped every longer
    maturity's search too, so 10-delta and 25-delta solved to the same
    clamped boundary and came out identical (found by inspecting a live
    AAPL matrix). Each slice searches its own range instead, scaled to its
    own SVI sigma (the parameter that sets its curvature scale).

    That width is bounded on both ends -- max() so a very tightly-fit short
    slice still gets a workable search range, min() so a loosely-fit one
    doesn't get pushed into SVI's asymptotically-linear wings, where total
    variance stops tracking the calibrated smile and delta can stop being
    monotonic in k at all (also found empirically: an 8-sigma width put
    several maturities' "10-delta" vol above 300%, and one maturity's four
    delta buckets all collapsed onto the same nonsensical value). Whatever
    solve_k_for_delta can't bracket inside this range comes back None and
    stays None all the way to the response -- a blank cell, not a guess.

    A bracket existing is not by itself proof the resulting vol is sane:
    delta can stay monotonic in k even where SVI's wings have gone
    asymptotically linear, so the bisection can still converge to a k whose
    implied vol is wildly larger than the slice's own ATM vol (found on a
    live AAPL chain: a 1-day slice with almost no liquid strikes solved a
    "10-delta" vol near 490%). _defensible_vol rejects any solved vol more
    than 5x the ATM vol for that same slice, turning it into None rather
    than a number nobody on a desk would trust.
    """
    width = min(max(4.0 * slc["sigma"], 0.5), 1.5)
    k_min, k_max = slc["m"] - width, slc["m"] + width

    k_25p = solve_k_for_delta(slc, -0.25, False, k_min, k_max)
    k_10p = solve_k_for_delta(slc, -0.10, False, k_min, k_max)
    k_25c = solve_k_for_delta(slc, 0.25, True, k_min, k_max)
    k_10c = solve_k_for_delta(slc, 0.10, True, k_min, k_max)

    vol_atm = svi_iv_at_k(0.0, slc)

    def _defensible_vol(k: Optional[float]) -> Optional[float]:
        if k is None:
            return None
        vol = svi_iv_at_k(k, slc)
        return vol if vol <= 5.0 * vol_atm else None

    vol_25p = _defensible_vol(k_25p)
    vol_10p = _defensible_vol(k_10p)
    vol_25c = _defensible_vol(k_25c)
    vol_10c = _defensible_vol(k_10c)

    rr25 = vol_25c - vol_25p if vol_25c is not None and vol_25p is not None else None
    bf25 = (
        0.5 * (vol_25c + vol_25p) - vol_atm
        if vol_25c is not None and vol_25p is not None
        else None
    )
    rr10 = vol_10c - vol_10p if vol_10c is not None and vol_10p is not None else None
    bf10 = (
        0.5 * (vol_10c + vol_10p) - vol_atm
        if vol_10c is not None and vol_10p is not None
        else None
    )

    return {
        "ttm": slc["ttm"],
        "tenor_label": tenor_label(slc["ttm"]),
        "vol_10p": vol_10p,
        "vol_25p": vol_25p,
        "vol_atm": vol_atm,
        "vol_25c": vol_25c,
        "vol_10c": vol_10c,
        "rr25": rr25,
        "bf25": bf25,
        "rr10": rr10,
        "bf10": bf10,
    }


def sabr_quotes_from_svi_slice(
    slc: dict, forward: float, k_min: float = -0.3, k_max: float = 0.3, n_points: int = 11
) -> List["qm.SABRSliceQuote"]:
    """Synthetic SABR-calibration quotes sampled off an already-calibrated,
    already arbitrage-checked SVI slice, rather than raw chain quotes
    directly: svi_quotes_from_raw_surface (market/svi_calibration.hpp) --
    the C++ equivalent that reads straight from RawVolSurface -- isn't bound
    to Python, and re-implementing RawVolSurface's cleaning logic here would
    duplicate it. Fitting SABR to the cleaned SVI curve instead means SABR
    inherits SVI's own liquidity/arbitrage filtering for free. `forward` is
    this slice's own forward (spot * exp((rate - dividend) * slc["ttm"])),
    used to turn each sampled log-moneyness k back into an actual strike."""
    quotes: List[qm.SABRSliceQuote] = []
    for i in range(n_points):
        k = k_min + i * (k_max - k_min) / max(n_points - 1, 1)
        w = _svi_total_variance(k, slc["a"], slc["b"], slc["rho"], slc["m"], slc["sigma"])
        if w <= 0:
            continue
        q = qm.SABRSliceQuote()
        q.strike = forward * math.exp(k)
        q.market_iv = math.sqrt(w / slc["ttm"])
        q.weight = 1.0
        quotes.append(q)
    return quotes


def svi_implied_vol_grid(
    slices: List[dict],
    spot: float,
    rate: float,
    dividend: float,
    k_min: float,
    k_max: float,
    n_strikes: int = 60,
    n_maturities: int = 40,
) -> tuple[List[float], List[float], List[List[Optional[float]]]]:
    """A regular (strike, maturity) grid of SVI-fitted implied vol, linearly
    interpolating total variance in T between adjacent calibrated slices --
    the arbitrage-checked replacement for the old bicubic-spline surface.
    """
    ordered = sorted(slices, key=lambda s: s["ttm"])
    t_lo, t_hi = ordered[0]["ttm"], ordered[-1]["ttm"]

    T_grid = [
        t_lo + i * (t_hi - t_lo) / max(n_maturities - 1, 1) for i in range(n_maturities)
    ]
    T_mid = 0.5 * (t_lo + t_hi)
    F_mid = spot * math.exp((rate - dividend) * T_mid)
    K_grid = [
        F_mid * math.exp(k_min + i * (k_max - k_min) / max(n_strikes - 1, 1))
        for i in range(n_strikes)
    ]

    def variance_at(k: float, T: float) -> float:
        T_clamped = min(max(T, t_lo), t_hi)
        i = 0
        while i + 2 < len(ordered) and ordered[i + 1]["ttm"] < T_clamped:
            i += 1
        s0, s1 = ordered[i], ordered[i + 1]
        lam = (T_clamped - s0["ttm"]) / (s1["ttm"] - s0["ttm"])
        w0 = _svi_total_variance(k, s0["a"], s0["b"], s0["rho"], s0["m"], s0["sigma"])
        w1 = _svi_total_variance(k, s1["a"], s1["b"], s1["rho"], s1["m"], s1["sigma"])
        return (1.0 - lam) * w0 + lam * w1

    values: List[List[Optional[float]]] = []
    for T in T_grid:
        F_T = spot * math.exp((rate - dividend) * T)
        row: List[Optional[float]] = []
        for K in K_grid:
            k = math.log(K / F_T)
            w = variance_at(k, T)
            row.append(round(math.sqrt(w / T), 6) if w > 0 else None)
        values.append(row)

    return K_grid, list(T_grid), values

