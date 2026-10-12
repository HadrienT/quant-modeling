"""The backtest runs: read, call the `quantbacktest` wheel, hand back
(blueprint/wp/24-backtest.md, lot B8).

Rule of the split with the quant-backtest repository: every computation
there, every input and output here. What this module does to a number is
limited to changes of unit and of shape:

- a basis-point cost into a fraction, FRED's and French's percents into
  decimals;
- a value series into daily returns, `value[t] / value[t-1] - 1`, and a return
  less the risk-free rate of the same day;
- row indices into dates, NaN into null.

A metric, an interval, a test or a weight computed here would be a missing
function of the wheel: it belongs in an issue of quant-backtest.

A run is a pure function of its request: no clock, no state, the seed is in the
request. That is what `replay.py` relies on.
"""

from __future__ import annotations

import math
import re
import time
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Any, Iterable, Optional

import numpy as np
import quantbacktest as qb

from . import market as mkt
from . import schemas as s

#: Threads handed to the engine's parallel calls. The result does not depend
#: on it (contract §3); the bound is this server's, shared with the pricers.
THREADS = 4

Error = qb.Error


def build() -> s.EngineBuild:
    return s.EngineBuild(**qb.build_info())


# ── Shapes ───────────────────────────────────────────────────────────────────


def _number(value: Any) -> Optional[float]:
    if value is None:
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def _numbers(values: Iterable[Any]) -> list[Optional[float]]:
    return [_number(v) for v in values]


def _days(values: np.ndarray) -> list[date]:
    return [d.astype(object) for d in values]


def _metric(raw: Optional[dict]) -> Optional[s.Metric]:
    if raw is None:
        return None
    return s.Metric(
        **{k: _number(raw[k]) for k in s.Metric.model_fields if k not in _TEXT},
        **{k: raw[k] for k in _TEXT},
    )


_TEXT = ("method", "convention")


def _returns(value: np.ndarray) -> np.ndarray:
    return value[1:] / value[:-1] - 1.0


# ── What is read ─────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class _Inputs:
    market: mkt.Market
    start_index: int
    cash_rate: np.ndarray  # simple rate of each period
    fingerprints: dict[str, str]

    @property
    def tables(self) -> dict:
        m = self.market
        return dict(
            open=m.open,
            high=m.high,
            low=m.low,
            volume=m.volume,
            cash_rate=self.cash_rate,
            dividends=m.dividends,
            splits=m.splits,
            start_index=self.start_index,
        )


def _fingerprint(m: mkt.Market) -> str:
    return qb.fingerprint(
        m.dates, m.close, m.open, m.high, m.low, m.volume, *m.dividends, *m.splits
    )


def _window(start: date, lookback: int) -> date:
    """First day to read: the estimation window ends on the first decision,
    so the bars before `start` are needed. Trading days into calendar days,
    with room for holidays; more than needed is harmless, `start_index` says
    where the book opens."""
    return start - timedelta(days=int(lookback * 1.6) + 15)


def _read(req: s._Window, lookback: int) -> _Inputs:
    market = mkt.market(req.tickers, _window(req.start, lookback), req.end)
    start_index = int(np.searchsorted(market.dates, np.datetime64(req.start)))
    if len(market.dates) - start_index < 60:
        raise mkt.MissingData(
            f"Fewer than 60 trading days between {req.start} and {req.end}."
        )
    annual = mkt.annual_cash_rate(market.dates)
    return _Inputs(
        market=market,
        start_index=start_index,
        cash_rate=qb.period_rates(market.dates, annual, basis=mkt.CASH_BASIS),
        fingerprints={
            "market": _fingerprint(market),
            "cash_rate": qb.fingerprint(annual),
        },
    )


def _benchmark(inputs: _Inputs) -> tuple[np.ndarray, str]:
    """Daily total return of the benchmark on the run's days."""
    dates = inputs.market.dates
    bench = mkt.market(
        (mkt.BENCHMARK,), dates[0].astype(object), dates[-1].astype(object)
    )
    rows = np.searchsorted(bench.dates, dates)
    rows[rows == len(bench.dates)] = 0
    absent = dates[bench.dates[rows] != dates]
    if len(absent):
        raise mkt.MissingData(
            f"The benchmark {mkt.BENCHMARK} has no quote on {absent[0]}."
        )
    total = qb.total_returns(
        bench.dates, bench.close, dividends=bench.dividends, splits=bench.splits
    )
    return total[rows, 0], _fingerprint(bench)


def _data(inputs: _Inputs, fingerprints: dict[str, str]) -> s.DataRead:
    dates = inputs.market.dates[inputs.start_index :]
    return s.DataRead(
        first=dates[0].astype(object),
        last=dates[-1].astype(object),
        days=len(dates),
        fingerprints=fingerprints,
    )


# ── Strategy ─────────────────────────────────────────────────────────────────


def _report(raw: dict) -> s.Report:
    return s.Report(
        observations=raw["observations"],
        years=raw["years"],
        metrics={name: _metric(m) for name, m in raw["metrics"].items()},
        omitted=dict(raw["omitted"]),
        t_statistic=_number(raw["t_statistic"]),
        probabilistic_sharpe_ratio=_number(raw["probabilistic_sharpe_ratio"]),
        minimum_track_record_years=_number(raw["minimum_track_record_years"]),
    )


def _quality(flags: list[dict], tickers: tuple[str, ...]) -> list[s.QualityFlag]:
    """The engine names an asset by its column; the page names it by its
    ticker."""

    def named(match: re.Match) -> str:
        return tickers[int(match.group(1))]

    return [
        s.QualityFlag(
            severity=f["severity"],
            ticker=tickers[f["asset"]] if 0 <= f["asset"] < len(tickers) else None,
            count=f["count"],
            message=re.sub(r"\basset (\d+)\b", named, f["message"]),
        )
        for f in flags
    ]


def _factor_regression(
    req: s.StrategyRequest, dates: np.ndarray, returns: np.ndarray
) -> tuple[s.FactorRegression, str]:
    """On the days the factors exist for: French's library is published with
    a lag, so the regression window ends before the run does. The excess
    return uses French's own risk-free rate, the one his factors are net of."""
    f = mkt.factors(
        req.factor_dataset, dates[0].astype(object), dates[-1].astype(object)
    )
    _, ours, theirs = np.intersect1d(dates, f.dates, return_indices=True)
    if len(ours) < 60:
        raise mkt.MissingData(
            f"Fewer than 60 days of {req.factor_dataset} factors in the window."
        )
    raw = qb.factor_regression(
        returns[ours] - f.risk_free[theirs],
        f.values[theirs],
        newey_west_lags=req.newey_west_lags,
    )
    regression = s.FactorRegression(
        dataset=f.dataset,
        factors=list(f.names),
        first=dates[ours[0]].astype(object),
        last=dates[ours[-1]].astype(object),
        alpha=_metric(raw["alpha"]),
        betas=[_metric(b) for b in raw["betas"]],
        alpha_t_statistic=_number(raw["alpha_t_statistic"]),
        beta_t_statistics=_numbers(raw["beta_t_statistics"]),
        r_squared=_number(raw["r_squared"]),
        residual_volatility=_number(raw["residual_volatility"]),
        observations=raw["observations"],
        newey_west_lags=raw["newey_west_lags"],
    )
    return regression, qb.fingerprint(f.dates, f.values, f.risk_free)


def run_strategy(req: s.StrategyRequest) -> s.StrategyResponse:
    began = time.perf_counter()
    inputs = _read(req, req.lookback)
    market = inputs.market
    benchmark, benchmark_print = _benchmark(inputs)

    result = qb.run_allocation(
        market.dates,
        market.close,
        method=req.method,
        lookback=req.lookback,
        rebalance=req.rebalance,
        initial_capital=req.initial_capital,
        proportional_cost=req.proportional_cost_bps / 1e4,
        max_weight=req.max_weight,
        timing=req.timing,
        **inputs.tables,
    )

    # Returns are on the days after the book opens.
    first = inputs.start_index + 1
    dates = market.dates[first:]
    cash = inputs.cash_rate[first:]
    benchmark = benchmark[first:]
    bootstrap = dict(
        bootstrap_replications=req.bootstrap_replications,
        mean_block_length=req.mean_block_length,
        seed=req.seed,
        threads=THREADS,
    )

    def run(name: str) -> Optional[s.Run]:
        book = result[name]
        if book is None:
            return None
        returns = _returns(book["value"])
        return s.Run(
            value=_numbers(book["value"]),
            returns=None,
            report=_report(
                qb.performance_report(returns, cash, benchmark, **bootstrap)
            ),
        )

    strategy = _returns(result["strategy"]["value"])
    equal_weight = _returns(result["equal_weight"]["value"])
    difference = qb.sharpe_difference(strategy - cash, equal_weight - cash, **bootstrap)
    regression, factors_print = _factor_regression(req, dates, strategy)

    allocations = result["allocations"]
    costs = result["cost_summary"]
    participation = costs["participation"]
    return s.StrategyResponse(
        tickers=list(market.tickers),
        dates=_days(market.dates[inputs.start_index :]),
        strategy=run("strategy"),
        gross=run("gross"),
        equal_weight=run("equal_weight"),
        buy_and_hold=run("buy_and_hold"),
        benchmark=s.Run(
            value=None,
            returns=_numbers(benchmark),
            report=_report(qb.performance_report(benchmark, cash, None, **bootstrap)),
        ),
        benchmark_ticker=mkt.BENCHMARK,
        allocations=s.Allocations(
            dates=_days(allocations["dates"]),
            weights=allocations["weights"].tolist(),
            eligible=allocations["eligible"].tolist(),
            in_sample_sharpe=_numbers(allocations["in_sample_sharpe"]),
            shrinkage=_numbers(allocations["shrinkage"]),
        ),
        cost_summary=s.CostSummary(
            annualized_turnover=_number(costs["annualized_turnover"]),
            annual_cost_bps=_number(costs["annual_cost_bps"]),
            break_even_cost=_metric(costs["break_even_cost"]),
            participation=participation
            and s.Participation(
                executions=participation["executions"],
                maximum=_number(participation["maximum"]),
                median=_number(participation["median"]),
            ),
        ),
        sharpe_difference=s.SharpeDifference(
            analytical=_metric(difference["analytical"]),
            z_statistic=_number(difference["z_statistic"]),
            p_value=_number(difference["p_value"]),
            correlation=_number(difference["correlation"]),
            bootstrap=_metric(difference["bootstrap"]),
            bootstrap_p_value=_number(difference["bootstrap_p_value"]),
        ),
        factor_regression=regression,
        conventions=dict(result["conventions"]),
        quality=_quality(result["quality"], market.tickers),
        rules=s.Rules(
            optimal_block_length=_number(qb.optimal_block_length(strategy)),
            newey_west_lag=qb.newey_west_lag(len(strategy)),
        ),
        data=_data(
            inputs,
            {
                **inputs.fingerprints,
                "benchmark": benchmark_print,
                "factors": factors_print,
            },
        ),
        engine=build(),
        compute_ms=(time.perf_counter() - began) * 1e3,
    )


# ── Validation ───────────────────────────────────────────────────────────────


def run_validation(req: s.ValidationRequest) -> s.ValidationResponse:
    began = time.perf_counter()
    inputs = _read(req, max(c.lookback for c in req.grid))
    market = inputs.market
    grid = qb.run_allocation_grid(
        market.dates,
        market.close,
        grid=[c.model_dump() for c in req.grid],
        initial_capital=req.initial_capital,
        proportional_cost=req.proportional_cost_bps / 1e4,
        timing=req.timing,
        threads=THREADS,
        **inputs.tables,
    )
    dates, table = grid["dates"], grid["returns"]

    forward = qb.walk_forward(table, train=req.train, test=req.test)
    purged = qb.combinatorial_purged_cv(
        table,
        groups=req.groups,
        test_groups=req.test_groups,
        purge=req.purge,
        embargo=req.embargo,
    )
    deflated = qb.deflated_sharpe(table)
    overfitting = qb.backtest_overfitting(table, blocks=req.blocks)

    def day(row: int) -> date:
        return dates[row].astype(object)

    return s.ValidationResponse(
        tickers=list(market.tickers),
        dates=_days(dates),
        descriptions=list(grid["descriptions"]),
        final_values=_numbers(grid["final_values"]),
        walk_forward=s.WalkForward(
            folds=[
                s.Fold(
                    train_begin=day(f["train_begin"]),
                    train_end=day(f["train_end"] - 1),  # ends are exclusive rows
                    test_begin=day(f["test_begin"]),
                    test_end=day(f["test_end"] - 1),
                    chosen=f["chosen"],
                    in_sample_sharpe=_number(f["in_sample_sharpe"]),
                    out_of_sample_sharpe=_number(f["out_of_sample_sharpe"]),
                )
                for f in forward["folds"]
            ],
            first=day(forward["first_row"]),
            returns=_numbers(forward["returns"]),
        ),
        purged_cv=s.PurgedCv(
            splits=purged["splits"],
            paths=purged["paths"],
            path_sharpes=_numbers(purged["path_sharpes"]),
            in_sample_sharpes=_numbers(purged["in_sample_sharpes"]),
            out_of_sample_sharpes=_numbers(purged["out_of_sample_sharpes"]),
            chosen=[int(c) for c in purged["chosen"]],
        ),
        deflated_sharpe=s.DeflatedSharpe(
            best=deflated["best"],
            sharpe=_number(deflated["sharpe"]),
            expected_maximum=_number(deflated["expected_maximum"]),
            sharpe_deviation=_number(deflated["sharpe_deviation"]),
            trials=deflated["trials"],
            deflated_sharpe_ratio=_number(deflated["deflated_sharpe_ratio"]),
        ),
        overfitting=s.Overfitting(
            probability=_number(overfitting["probability"]),
            splits=overfitting["splits"],
            logits=_numbers(overfitting["logits"]),
            mean_in_sample_sharpe=_number(overfitting["mean_in_sample_sharpe"]),
            mean_out_of_sample_sharpe=_number(overfitting["mean_out_of_sample_sharpe"]),
        ),
        data=_data(inputs, inputs.fingerprints),
        engine=build(),
        compute_ms=(time.perf_counter() - began) * 1e3,
    )
