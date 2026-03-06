"""Portfolio backtesting endpoint.

Optimises a portfolio of stocks/ETFs/indices using Sharpe-ratio maximisation
over a historical look-back window, then tracks the portfolio value forward
from the investment start date. Supports periodic rebalancing.
"""

from __future__ import annotations

import os
from datetime import date, datetime
from typing import List, Optional

import numpy as np
import pandas as pd
import requests
from fastapi import APIRouter, Depends, HTTPException
from google.cloud import bigquery
from pydantic import BaseModel, Field
from scipy.optimize import minimize
from starlette.concurrency import run_in_threadpool

from ..cache import TTLCache
from ..dependencies import bq_project_id, bq_table_ref, require_api_key
from ..logging_utils import get_logger

router = APIRouter(prefix="/api/backtest", tags=["backtest"])
logger = get_logger()

# ── Caches ────────────────────────────────────────────────────────────────────

# Keyed by comma-joined sorted tickers; holds the fully-pivoted price DataFrame
_PRICES_CACHE: TTLCache[str, pd.DataFrame] = TTLCache(max_size=64, ttl_seconds=60 * 30)

# Cached risk-free rate (10Y Treasury CMT from FRED)
_RF_RATE_CACHE: TTLCache[str, float] = TTLCache(max_size=1, ttl_seconds=60 * 60)

# ── Pydantic models ───────────────────────────────────────────────────────────

_SP500_TICKER = "^GSPC"


class BacktestRequest(BaseModel):
    tickers: List[str] = Field(..., min_length=1, max_length=30)
    opt_start: date = Field(..., description="Start of look-back / optimisation window")
    opt_end: date = Field(..., description="End of optimisation window and start of investment")
    initial_capital: float = Field(10_000.0, gt=0)
    max_share: float = Field(0.4, gt=0, le=1.0, description="Maximum weight for any single asset")
    min_share: float = Field(0.0, ge=0, lt=1.0, description="Minimum weight threshold; assets below are dropped")
    rebalance_freq: int = Field(
        0, ge=0, description="Rebalance every N business days. 0 = static allocation"
    )


class AllocationRow(BaseModel):
    ticker: str
    weight: float
    start_price: float
    end_price: float
    return_pct: float


class PortfolioPoint(BaseModel):
    date: str
    value: float


class BacktestMetrics(BaseModel):
    ath: float
    atl: float
    total_return_pct: float
    annualized_return_pct: float
    max_drawdown: float
    sharpe_ratio: float
    optimal_sharpe: float
    alpha: Optional[float] = None
    beta: Optional[float] = None


class BacktestResponse(BaseModel):
    opt_start: str
    opt_end: str
    allocation: List[AllocationRow]
    portfolio_values: List[PortfolioPoint]
    sp500_values: Optional[List[PortfolioPoint]] = None
    metrics: BacktestMetrics
    warnings: List[str]


# ── Internal helpers ──────────────────────────────────────────────────────────


def _fetch_risk_free_rate() -> float:
    """Fetch the latest 10Y Treasury rate from FRED.  Returns 0.04 as fallback."""
    cached = _RF_RATE_CACHE.get("rf")
    if cached is not None:
        return cached

    api_key = os.getenv("FRED_API_KEY")
    if not api_key:
        logger.warning("backtest: FRED_API_KEY not set, using fallback rf=0.04")
        return 0.04

    try:
        resp = requests.get(
            "https://api.stlouisfed.org/fred/series/observations",
            params={
                "series_id": "DGS10",
                "api_key": api_key,
                "file_type": "json",
                "sort_order": "desc",
                "limit": 5,
            },
            timeout=10,
        )
        resp.raise_for_status()
        observations = resp.json().get("observations", [])
        for obs in observations:
            val_str = obs.get("value", ".")
            if val_str != ".":
                rate = float(val_str) / 100.0
                _RF_RATE_CACHE.set("rf", rate)
                return rate
    except Exception as exc:
        logger.warning("backtest: failed to fetch FRED rate", extra={"error": str(exc)})

    return 0.04


def _load_prices(tickers: List[str], project_id: str, table_ref: str) -> pd.DataFrame:
    """Fetch and pivot close prices from BigQuery for the given tickers."""
    all_tickers = sorted(set(tickers) | {_SP500_TICKER})
    cache_key = ",".join(all_tickers)
    cached = _PRICES_CACHE.get(cache_key)
    if cached is not None:
        return cached

    # Use UNNEST with array parameter to avoid any string interpolation.
    # We still need the table ref in the query string (it's from our own config,
    # not user input), but ticker values go through query parameters.
    query = f"""
    SELECT Date, Ticker, Close
    FROM `{table_ref}`
    WHERE Ticker IN UNNEST(@tickers)
    ORDER BY Date ASC
    """
    job_config = bigquery.QueryJobConfig(
        query_parameters=[
            bigquery.ArrayQueryParameter("tickers", "STRING", all_tickers),
        ]
    )
    try:
        client = bigquery.Client(project=project_id)
        rows = list(client.query(query, job_config=job_config).result())
        df = pd.DataFrame(
            [(row["Date"], row["Ticker"], row["Close"]) for row in rows],
            columns=["Date", "Ticker", "Close"],
        )
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"BigQuery error: {exc}") from exc

    if df.empty:
        raise HTTPException(status_code=404, detail="No price data found for the requested tickers")

    prices = df.pivot(index="Date", columns="Ticker", values="Close")
    prices = prices.infer_objects().interpolate(method="linear")
    prices.index = pd.to_datetime(prices.index).tz_localize("UTC")
    prices.sort_index(inplace=True)

    _PRICES_CACHE.set(cache_key, prices)
    return prices


def _log_returns(prices: pd.DataFrame) -> pd.DataFrame:
    """Compute daily log-returns, keeping only rows where the market is open."""
    # Filter to rows that are trading days (at least one ticker has a non-NaN close)
    trading_day_mask = prices.notna().any(axis=1)
    p = prices[trading_day_mask]
    returns = np.log(p / p.shift(1))
    return returns.dropna(how="all")


def _snap_to_available(ts: pd.Timestamp, index: pd.DatetimeIndex) -> pd.Timestamp:
    """Return the nearest available date <= ts from the index."""
    candidates = index[index <= ts]
    if candidates.empty:
        candidates = index[index >= ts]
    if candidates.empty:
        raise ValueError("No available dates in price index")
    return candidates[-1]


def _negative_sharpe(
    weights: np.ndarray,
    mean_returns: np.ndarray,
    cov: np.ndarray,
    rf_daily: float,
) -> float:
    ret = float(np.dot(weights, mean_returns))
    vol = float(np.sqrt(weights @ cov @ weights))
    if vol < 1e-12:
        return 0.0
    return -(ret - rf_daily) / vol


def _optimise_weights(
    returns: pd.DataFrame,
    rf_daily: float,
    max_share: float,
) -> np.ndarray:
    n = returns.shape[1]
    mean_ret = returns.mean().values
    cov = returns.cov().values
    x0 = np.full(n, 1.0 / n)
    bounds = [(0.0, max_share)] * n
    constraints = {"type": "eq", "fun": lambda w: np.sum(w) - 1.0}
    result = minimize(
        _negative_sharpe,
        x0,
        args=(mean_ret, cov, rf_daily),
        method="SLSQP",
        bounds=bounds,
        constraints=constraints,
        options={"ftol": 1e-9, "maxiter": 500},
    )
    return result.x, -result.fun


def _filter_weights(
    weights: np.ndarray,
    tickers: List[str],
    min_share: float,
) -> tuple[List[str], np.ndarray]:
    """Remove assets below min_share and renormalise."""
    mask = weights >= max(min_share, 1e-4)
    if not mask.any():
        mask = weights == weights.max()
    filtered_w = weights[mask]
    filtered_w = filtered_w / filtered_w.sum()
    filtered_t = [tickers[i] for i in range(len(tickers)) if mask[i]]
    order = np.argsort(filtered_w)[::-1]
    return [filtered_t[i] for i in order], filtered_w[order]


def _track_value(
    prices: pd.DataFrame,
    tickers: List[str],
    weights: np.ndarray,
    capital: float,
    start: pd.Timestamp,
    end: Optional[pd.Timestamp] = None,
) -> pd.Series:
    """Track the portfolio value from *start* to *end* given fixed weights."""
    p = prices[tickers].loc[start:]
    if end is not None:
        p = p.loc[:end]
    p = p.dropna(how="all")
    if p.empty:
        return pd.Series(dtype=float)
    init_prices = p.iloc[0]
    shares = (weights * capital) / init_prices
    values = p.dot(shares)
    return values


def _compute_metrics(
    portfolio_values: pd.Series,
    rf_annual: float,
) -> dict:
    v = portfolio_values.dropna()
    if len(v) < 2:
        return {}
    ath = float(v.max())
    atl = float(v.min())
    total_ret = float((v.iloc[-1] / v.iloc[0] - 1) * 100)
    num_days = (v.index[-1] - v.index[0]).days
    ann_ret = float(((v.iloc[-1] / v.iloc[0]) ** (365.0 / max(num_days, 1)) - 1) * 100)
    running_max = v.cummax()
    max_dd = float((v / running_max - 1).min())
    log_rets = np.log(v / v.shift(1)).dropna()
    rf_daily = rf_annual / 252.0
    excess = log_rets - rf_daily
    sharpe = float(excess.mean() / excess.std() * np.sqrt(252)) if excess.std() > 0 else 0.0
    return {
        "ath": ath,
        "atl": atl,
        "total_return_pct": total_ret,
        "annualized_return_pct": ann_ret,
        "max_drawdown": max_dd,
        "sharpe_ratio": sharpe,
    }


def _capm(
    portfolio_values: pd.Series,
    sp500_prices: pd.Series,
    rf_annual: float,
) -> tuple[Optional[float], Optional[float]]:
    """Compute CAPM alpha and beta via OLS."""
    port_ret = np.log(portfolio_values / portfolio_values.shift(1)).dropna()
    sp_ret = np.log(sp500_prices / sp500_prices.shift(1)).dropna()
    common = port_ret.index.intersection(sp_ret.index)
    if len(common) < 20:
        return None, None
    rf_daily = rf_annual / 252.0
    port_excess = (port_ret.loc[common] - rf_daily).values
    mkt_excess = (sp_ret.loc[common] - rf_daily).values
    X = np.column_stack([np.ones(len(mkt_excess)), mkt_excess])
    coeffs, _, _, _ = np.linalg.lstsq(X, port_excess, rcond=None)
    return float(coeffs[0]), float(coeffs[1])


def _run_backtest(req: BacktestRequest) -> BacktestResponse:
    warnings: List[str] = []

    project_id = bq_project_id()
    table_ref = bq_table_ref()
    prices = _load_prices(req.tickers, project_id, table_ref)

    tickers_in_data = [t for t in req.tickers if t in prices.columns]
    missing = set(req.tickers) - set(tickers_in_data)
    if missing:
        warnings.append(f"Tickers not found in data and excluded: {sorted(missing)}")
    if not tickers_in_data:
        raise HTTPException(status_code=422, detail="None of the requested tickers have data")

    rf_annual = _fetch_risk_free_rate()
    rf_daily = rf_annual / 252.0

    opt_start_ts = pd.Timestamp(req.opt_start).tz_localize("UTC")
    opt_end_ts = pd.Timestamp(req.opt_end).tz_localize("UTC")

    # Snap dates to nearest available trading day
    idx = prices.index
    opt_start_ts = _snap_to_available(opt_start_ts, idx)
    opt_end_ts = _snap_to_available(opt_end_ts, idx)

    if opt_start_ts >= opt_end_ts:
        raise HTTPException(status_code=422, detail="opt_start must be before opt_end")

    if opt_start_ts != pd.Timestamp(req.opt_start).tz_localize("UTC"):
        warnings.append(f"opt_start snapped from {req.opt_start} to {opt_start_ts.date()}")
    if opt_end_ts != pd.Timestamp(req.opt_end).tz_localize("UTC"):
        warnings.append(f"opt_end snapped from {req.opt_end} to {opt_end_ts.date()}")

    # ── Compute log-returns for the optimisation window ──────────────────────
    returns = _log_returns(prices[tickers_in_data])
    opt_returns = returns.loc[opt_start_ts:opt_end_ts].dropna(how="all")

    if opt_returns.shape[0] < 5:
        raise HTTPException(
            status_code=422,
            detail="Not enough trading days in the optimisation window (min 5 required)",
        )

    # Drop columns with too many NaNs (< 50% valid)
    valid_col_mask = opt_returns.notna().mean() >= 0.5
    dropped_cols = [c for c in tickers_in_data if not valid_col_mask[c]]
    if dropped_cols:
        warnings.append(f"Insufficient history in opt window, excluded: {dropped_cols}")
    opt_returns = opt_returns.loc[:, valid_col_mask].dropna()
    tickers_in_data = list(opt_returns.columns)
    if not tickers_in_data:
        raise HTTPException(status_code=422, detail="No tickers have sufficient data in the optimisation window")

    # ── Portfolio optimisation ────────────────────────────────────────────────
    raw_weights, optimal_sharpe = _optimise_weights(opt_returns, rf_daily, req.max_share)
    sorted_tickers, sorted_weights = _filter_weights(raw_weights, tickers_in_data, req.min_share)
    optimal_sharpe_ann = optimal_sharpe * np.sqrt(252)

    # ── Run backtest ──────────────────────────────────────────────────────────
    if req.rebalance_freq == 0:
        portfolio_values = _track_value(
            prices, sorted_tickers, sorted_weights, req.initial_capital, opt_end_ts
        )
    else:
        portfolio_values = _run_rebalanced_backtest(
            prices=prices,
            tickers=sorted_tickers,
            initial_capital=req.initial_capital,
            start=opt_end_ts,
            rf_daily=rf_daily,
            max_share=req.max_share,
            min_share=req.min_share,
            rebalance_freq=req.rebalance_freq,
            initial_weights=sorted_weights,
        )

    if portfolio_values.empty:
        raise HTTPException(status_code=422, detail="No price data available after the investment start date")

    # ── Allocation summary (at investment start date) ─────────────────────────
    invest_start = portfolio_values.index[0]
    invest_end = portfolio_values.index[-1]
    allocation: List[AllocationRow] = []
    for ticker, weight in zip(sorted_tickers, sorted_weights):
        if ticker not in prices.columns:
            continue
        col = prices[ticker]
        start_p = float(col.loc[invest_start]) if invest_start in col.index else float("nan")
        end_p = float(col.loc[invest_end]) if invest_end in col.index else float("nan")
        ret_pct = (end_p / start_p - 1) * 100 if start_p and end_p else float("nan")
        allocation.append(
            AllocationRow(
                ticker=ticker,
                weight=float(weight),
                start_price=round(start_p, 4),
                end_price=round(end_p, 4),
                return_pct=round(ret_pct, 2),
            )
        )

    # ── Metrics ───────────────────────────────────────────────────────────────
    metrics_dict = _compute_metrics(portfolio_values, rf_annual)
    metrics_dict["optimal_sharpe"] = float(optimal_sharpe_ann)

    # CAPM vs S&P 500
    alpha, beta = None, None
    if _SP500_TICKER in prices.columns:
        sp500_col = prices[_SP500_TICKER].loc[invest_start:invest_end].dropna()
        alpha, beta = _capm(portfolio_values, sp500_col, rf_annual)

    metrics_dict["alpha"] = alpha
    metrics_dict["beta"] = beta

    # ── S&P 500 benchmark series (rebased) ────────────────────────────────────
    sp500_values: Optional[List[PortfolioPoint]] = None
    if _SP500_TICKER in prices.columns:
        sp_raw = prices[_SP500_TICKER].loc[invest_start:invest_end].dropna()
        if not sp_raw.empty:
            rebased = sp_raw / sp_raw.iloc[0] * req.initial_capital
            sp500_values = [
                PortfolioPoint(date=str(d.date()), value=round(v, 4))
                for d, v in rebased.items()
            ]

    pv_list = [
        PortfolioPoint(date=str(d.date()), value=round(v, 4))
        for d, v in portfolio_values.items()
    ]

    return BacktestResponse(
        opt_start=str(opt_start_ts.date()),
        opt_end=str(opt_end_ts.date()),
        allocation=allocation,
        portfolio_values=pv_list,
        sp500_values=sp500_values,
        metrics=BacktestMetrics(**metrics_dict),
        warnings=warnings,
    )


def _run_rebalanced_backtest(
    prices: pd.DataFrame,
    tickers: List[str],
    initial_capital: float,
    start: pd.Timestamp,
    rf_daily: float,
    max_share: float,
    min_share: float,
    rebalance_freq: int,
    initial_weights: np.ndarray,
) -> pd.Series:
    """Track portfolio with periodic rebalancing."""
    invest_prices = prices[tickers].loc[start:].dropna(how="all")
    if invest_prices.empty:
        return pd.Series(dtype=float)

    trading_days = invest_prices.index.tolist()
    rebalance_dates = set(trading_days[::rebalance_freq])

    current_weights = initial_weights.copy()
    current_tickers = tickers[:]
    capital = initial_capital
    last_rebalance = trading_days[0]

    all_values: List[float] = []
    all_dates: List[pd.Timestamp] = []

    for i, current_date in enumerate(trading_days):
        if i == 0 or current_date in rebalance_dates:
            # Re-optimise on returns up to current_date
            hist_returns = _log_returns(prices[tickers]).loc[:current_date].dropna(how="all")
            if len(hist_returns) >= 10:
                valid_mask = hist_returns.notna().mean() >= 0.5
                valid_hist = hist_returns.loc[:, valid_mask].dropna()
                if valid_hist.shape[1] > 0 and valid_hist.shape[0] >= 5:
                    raw_w, _ = _optimise_weights(valid_hist, rf_daily, max_share)
                    new_tickers, new_weights = _filter_weights(
                        raw_w, list(valid_hist.columns), min_share
                    )
                    # Track value until previous rebalance to get current capital
                    if i > 0:
                        tracked = _track_value(
                            prices, current_tickers, current_weights, capital,
                            last_rebalance, current_date,
                        )
                        if not tracked.empty:
                            all_values.extend(tracked.values[1:])
                            all_dates.extend(tracked.index[1:])
                            capital = float(tracked.iloc[-1])
                    current_tickers = new_tickers
                    current_weights = new_weights
                    last_rebalance = current_date

    # Track from last rebalance to end
    final_segment = _track_value(
        prices, current_tickers, current_weights, capital, last_rebalance
    )
    if not final_segment.empty:
        if all_values:
            all_values.extend(final_segment.values[1:])
            all_dates.extend(final_segment.index[1:])
        else:
            all_values.extend(final_segment.values)
            all_dates.extend(final_segment.index)

    return pd.Series(all_values, index=all_dates).sort_index()


# ── Endpoint ──────────────────────────────────────────────────────────────────


@router.post("/run", response_model=BacktestResponse, dependencies=[Depends(require_api_key)])
async def run_backtest(req: BacktestRequest) -> BacktestResponse:
    """
    Optimise a portfolio of assets over a historical window, then track its
    performance forward. Returns allocation, portfolio value time series,
    performance metrics, and CAPM alpha/beta vs S&P 500.
    """
    logger.info(
        "backtest_run",
        extra={
            "tickers": req.tickers,
            "opt_start": str(req.opt_start),
            "opt_end": str(req.opt_end),
            "rebalance_freq": req.rebalance_freq,
        },
    )
    try:
        return await run_in_threadpool(_run_backtest, req)
    except HTTPException:
        raise
    except Exception as exc:
        logger.error("backtest_run_failed", extra={"error": str(exc)})
        raise HTTPException(status_code=500, detail=f"Backtest failed: {exc}") from exc
