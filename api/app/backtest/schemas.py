"""Request and response models of the backtest endpoints
(blueprint/wp/24-backtest.md, lot B8).

A parameter the engine has no default for is a field of the request with no
default either: the cost, the look-back, the rebalancing rule, the three
bootstrap arguments and the Newey-West lags are chosen by whoever runs the
backtest, shown on the page and recorded with the run.
"""

from __future__ import annotations

from datetime import date
from typing import Literal, Optional, Union

from pydantic import BaseModel, Field, field_validator, model_validator

Method = Literal["minimum_variance", "maximum_sharpe", "risk_parity", "equal_weight"]
Timing = Literal["next_close", "next_open"]
Rebalance = Union[Literal["monthly"], int]

#: Methods that take bounds on the weights; the engine refuses a bound on the
#: other two (a bounded portfolio has no equal weights or risk contributions).
BOUNDED_METHODS = ("minimum_variance", "maximum_sharpe")


class Metric(BaseModel):
    """A figure as the engine returns it: never without its interval, the
    method that produced the interval, and its convention."""

    value: Optional[float]
    standard_error: Optional[float]
    lower: Optional[float]
    upper: Optional[float]
    confidence: Optional[float]
    method: str
    convention: str


class _Window(BaseModel):
    tickers: list[str] = Field(..., min_length=2, max_length=30)
    start: date = Field(..., description="First day the strategy is invested")
    end: date
    initial_capital: float = Field(..., gt=0, le=1e12)
    proportional_cost_bps: float = Field(
        ...,
        ge=0,
        le=500,
        description="Cost of a trade, in basis points of the amount traded",
    )
    timing: Timing = "next_close"

    @field_validator("tickers")
    @classmethod
    def _tickers(cls, value: list[str]) -> list[str]:
        cleaned = [t.strip().upper() for t in value]
        if any(not t or len(t) > 12 for t in cleaned):
            raise ValueError("a ticker is 1 to 12 characters")
        if len(set(cleaned)) != len(cleaned):
            raise ValueError("a ticker is listed twice")
        return cleaned

    @model_validator(mode="after")
    def _dates(self):
        if self.end <= self.start:
            raise ValueError("end must be after start")
        return self


class Configuration(BaseModel):
    method: Method
    lookback: int = Field(..., ge=20, le=1260, description="Trading days")
    rebalance: Rebalance = Field(
        ..., description='"monthly", or a number of trading days'
    )
    max_weight: float = Field(1.0, gt=0, le=1)

    @field_validator("rebalance")
    @classmethod
    def _rebalance(cls, value: Rebalance) -> Rebalance:
        if isinstance(value, int) and not 1 <= value <= 252:
            raise ValueError("rebalance is between 1 and 252 trading days")
        return value


class StrategyRequest(_Window, Configuration):
    bootstrap_replications: int = Field(..., ge=0, le=10_000)
    mean_block_length: float = Field(..., ge=1, le=250)
    seed: int = Field(..., ge=0, le=2**31 - 1)
    newey_west_lags: int = Field(..., ge=0, le=60)
    factor_dataset: Literal["ff3", "ff5"] = "ff3"


class ValidationRequest(_Window):
    """Every configuration tried is in `grid`: the number of columns is the
    number of trials the deflated Sharpe ratio corrects for."""

    grid: list[Configuration] = Field(..., min_length=2, max_length=48)
    train: int = Field(..., ge=60, le=2520, description="Walk-forward: choosing window")
    test: int = Field(..., ge=20, le=1260, description="Walk-forward: applied period")
    groups: int = Field(..., ge=3, le=16)
    test_groups: int = Field(..., ge=1, le=8)
    purge: int = Field(..., ge=0, le=252)
    embargo: int = Field(..., ge=0, le=252)
    blocks: int = Field(..., ge=4, le=20)

    @model_validator(mode="after")
    def _shape(self):
        if self.blocks % 2:
            raise ValueError("blocks must be even")
        if self.test_groups >= self.groups:
            raise ValueError("test_groups must be below groups")
        return self


# ── Universe ─────────────────────────────────────────────────────────────────


class Asset(BaseModel):
    ticker: str
    name: str
    first: date
    last: date


class EngineBuild(BaseModel):
    version: str
    git_sha: str
    quantmodeling_sha: str


class UniverseResponse(BaseModel):
    etfs: list[Asset]
    stocks: list[Asset]
    benchmark: str
    cash_series: str
    first: date
    last: date
    engine: EngineBuild


# ── Strategy ─────────────────────────────────────────────────────────────────


class Report(BaseModel):
    observations: int
    years: float
    metrics: dict[str, Metric]
    #: A metric that could not be bracketed, with the reason: absent, not zero.
    omitted: dict[str, str]
    t_statistic: Optional[float]
    probabilistic_sharpe_ratio: Optional[float]
    minimum_track_record_years: Optional[float]


class Run(BaseModel):
    #: A book: its value on each day of `dates`. The benchmark is not a book
    #: but a return series, one per day after the first; exactly one is set.
    value: Optional[list[float]]
    returns: Optional[list[Optional[float]]]
    report: Report


class Allocations(BaseModel):
    dates: list[date]
    weights: list[list[float]]
    eligible: list[int]
    #: Measured on the estimation window: a maximum taken over the data that
    #: measure it, not a forecast.
    in_sample_sharpe: list[Optional[float]]
    shrinkage: list[Optional[float]]


class Participation(BaseModel):
    executions: int
    maximum: Optional[float]
    median: Optional[float]


class CostSummary(BaseModel):
    annualized_turnover: Optional[float]
    annual_cost_bps: Optional[float]
    break_even_cost: Optional[Metric]
    participation: Optional[Participation]


class QualityFlag(BaseModel):
    severity: str
    ticker: Optional[str]
    count: int
    message: str


class SharpeDifference(BaseModel):
    analytical: Metric
    z_statistic: Optional[float]
    p_value: Optional[float]
    correlation: Optional[float]
    bootstrap: Optional[Metric]
    bootstrap_p_value: Optional[float]


class FactorRegression(BaseModel):
    dataset: str
    factors: list[str]
    first: date
    last: date
    alpha: Metric
    betas: list[Metric]
    alpha_t_statistic: Optional[float]
    beta_t_statistics: list[Optional[float]]
    r_squared: Optional[float]
    residual_volatility: Optional[float]
    observations: int
    newey_west_lags: int


class Rules(BaseModel):
    """What a rule would propose for two fields the request sets itself."""

    optimal_block_length: Optional[float]
    newey_west_lag: int


class DataRead(BaseModel):
    first: date
    last: date
    days: int
    #: SHA-256 of each table handed to the engine, in the order it was read.
    fingerprints: dict[str, str]


class StrategyResponse(BaseModel):
    tickers: list[str]
    dates: list[date]
    strategy: Run
    gross: Optional[Run]
    equal_weight: Run
    buy_and_hold: Run
    benchmark: Run
    benchmark_ticker: str
    allocations: Allocations
    cost_summary: CostSummary
    sharpe_difference: SharpeDifference
    factor_regression: FactorRegression
    conventions: dict[str, str]
    quality: list[QualityFlag]
    rules: Rules
    data: DataRead
    engine: EngineBuild
    compute_ms: float


# ── Validation ───────────────────────────────────────────────────────────────


class Fold(BaseModel):
    train_begin: date
    train_end: date
    test_begin: date
    test_end: date
    chosen: int
    in_sample_sharpe: Optional[float]
    out_of_sample_sharpe: Optional[float]


class WalkForward(BaseModel):
    folds: list[Fold]
    first: date
    returns: list[Optional[float]]


class PurgedCv(BaseModel):
    splits: int
    paths: int
    path_sharpes: list[Optional[float]]
    in_sample_sharpes: list[Optional[float]]
    out_of_sample_sharpes: list[Optional[float]]
    chosen: list[int]


class DeflatedSharpe(BaseModel):
    best: int
    sharpe: Optional[float]
    expected_maximum: Optional[float]
    sharpe_deviation: Optional[float]
    trials: int
    deflated_sharpe_ratio: Optional[float]


class Overfitting(BaseModel):
    #: A frequency over overlapping splits: no standard error exists for it.
    probability: Optional[float]
    splits: int
    logits: list[Optional[float]]
    mean_in_sample_sharpe: Optional[float]
    mean_out_of_sample_sharpe: Optional[float]


class ValidationResponse(BaseModel):
    tickers: list[str]
    dates: list[date]
    descriptions: list[str]
    final_values: list[Optional[float]]
    walk_forward: WalkForward
    purged_cv: PurgedCv
    deflated_sharpe: DeflatedSharpe
    overfitting: Overfitting
    data: DataRead
    engine: EngineBuild
    compute_ms: float
