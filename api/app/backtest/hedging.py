"""The hedging tab (blueprint/wp/24-backtest.md, lot B8): an option bought
and delta-hedged in a simulated world, each hedge on the same paths.

The world is named and given by its parameters, typed or calibrated: a Heston
world can be the one fitted to a ticker's stored option chain
(stochastic_vol.py, the same fit the pricing page uses). No object of the
`quantmodeling` wheel crosses into the engine: it receives a dictionary.

As everywhere in `backtest/`, every computation is the wheel's
(`hedging_backtest`, `fit_minimum_variance_delta`). What this module does to
the numbers: the strike as a multiple of the spot into currency, and the paths'
P&L into a histogram on common bins for the chart (a change of shape: 20 000
numbers per hedge do not travel to a browser).
"""

from __future__ import annotations

import math
import time
from datetime import date
from typing import Any, Literal, Optional

import numpy as np
import quantbacktest as qb
from pydantic import BaseModel, Field, model_validator

from ..valuation import value_hash
from . import engine
from . import schemas as s
from .tasks import Cancelled, Task

HedgeName = Literal[
    "black_scholes_fixed",
    "black_scholes_implied",
    "skew_adjusted",
    "minimum_variance",
    "true_model",
]

LABELS: dict[str, str] = {
    "black_scholes_fixed": "Black-Scholes delta, one fixed volatility",
    "black_scholes_implied": "Black-Scholes delta at each date's implied volatility",
    "skew_adjusted": "Skew-adjusted delta",
    "minimum_variance": "Minimum-variance delta (Hull & White, 2017)",
    "true_model": "The world's own delta",
}

#: Paths at most per world: a Heston world prices the option on every path at
#: every date (about 60 s for 20 000 paths and the true model's delta on four
#: threads of the development server, twice that on production's two CPUs).
MAX_PATHS = {"black_scholes": 50_000, "heston": 10_000}
HISTOGRAM_BINS = 60


class HedgingWorld(BaseModel):
    """The market the paths follow. With `ticker`, a Heston world calibrated on
    the stored chain of that ticker (spot, dividend and the five parameters
    from the database); otherwise the parameters typed."""

    model: Literal["black_scholes", "heston"] = "heston"
    ticker: Optional[str] = Field(None, min_length=1, max_length=12)
    spot: float = Field(100.0, gt=0)
    rate: float = Field(0.04, ge=-0.05, le=0.25, description="Continuous, per year.")
    dividend_yield: float = Field(0.0, ge=0, le=0.25)
    volatility: float = Field(0.2, gt=0, le=2, description="Black-Scholes world.")
    v0: float = Field(0.04, gt=0, le=4)
    kappa: float = Field(2.0, gt=0, le=50)
    theta: float = Field(0.04, gt=0, le=4)
    xi: float = Field(0.5, gt=0, le=5)
    rho: float = Field(-0.7, ge=-1, le=1)

    @model_validator(mode="after")
    def _ticker_is_heston(self) -> "HedgingWorld":
        if self.ticker and self.model != "heston":
            raise ValueError("a world calibrated on a ticker is a Heston world")
        return self


class HedgingRequest(BaseModel):
    world: HedgingWorld = Field(default_factory=HedgingWorld)
    strike: float = Field(1.0, ge=0.5, le=1.5, description="As a multiple of the spot.")
    maturity: float = Field(0.5, ge=0.05, le=2.0, description="Years.")
    is_call: bool = True
    hedges: list[HedgeName] = Field(
        default_factory=lambda: [
            "black_scholes_implied",
            "skew_adjusted",
            "true_model",
        ],
        min_length=1,
        max_length=5,
    )
    hedge_volatility: Optional[float] = Field(
        None,
        gt=0,
        le=2,
        description="The fixed volatility of black_scholes_fixed. Default: the "
        "world's volatility today (sqrt(v0) in a Heston world).",
    )
    rebalances: int = Field(50, ge=4, le=252)
    paths: int = Field(5_000, ge=1_000, le=50_000)
    seed: int = Field(1, ge=1)
    cost_rate: float = Field(
        0.0, ge=0, le=0.01, description="Proportional cost of each rebalancing trade."
    )
    confidence: float = Field(0.95, gt=0.5, lt=1)

    @model_validator(mode="after")
    def _check(self) -> "HedgingRequest":
        if len(set(self.hedges)) != len(self.hedges):
            raise ValueError("each hedge at most once")
        cap = MAX_PATHS[self.world.model]
        if self.paths > cap:
            raise ValueError(f"at most {cap:,} paths in a {self.world.model} world")
        return self


class HedgeResult(BaseModel):
    hedge: str
    label: str
    mean: s.Metric
    standard_deviation: s.Metric
    skewness: Optional[float]
    kurtosis: Optional[float]
    quantiles: dict[str, Optional[float]]
    step_error_autocorrelation: Optional[float]
    discretisation_noise: Optional[float] = Field(
        None, description="Derman and Kamal's floor for this number of rebalancings."
    )
    gamma_integral: Optional[s.Metric] = Field(
        None,
        description="black_scholes_fixed only: the P&L's expected gamma-weighted "
        "variance term, with its error.",
    )
    explanation: dict[str, s.Metric]
    unexplained_step_deviation: Optional[float]
    option_step_deviation: Optional[float]
    uncalibrated_steps: int = Field(
        ...,
        description="Dates where the implied volatility could not be read: not an error.",
    )
    convention: str
    histogram: list[int] = Field(
        ..., description="Paths per bin of HedgingResponse.bins."
    )


class HedgingData(BaseModel):
    source: str
    snapshot: Optional[date] = None
    fingerprints: dict[str, str]


class HedgingResponse(BaseModel):
    world: dict[str, Any] = Field(..., description="The world handed to the engine.")
    strike: float = Field(..., description="In currency.")
    maturity: float
    is_call: bool
    premium: float
    hedges: list[HedgeResult]
    bins: list[float] = Field(
        ..., description="Common bin edges of the P&L histograms."
    )
    coefficients: Optional[dict[str, float]] = Field(
        None,
        description="The minimum-variance delta's a, b, c, fitted on another seed.",
    )
    data: HedgingData
    engine: s.EngineBuild
    compute_ms: float


def _world(req: HedgingRequest, today: date) -> tuple[dict[str, Any], HedgingData]:
    w = req.world
    if w.ticker:
        from .. import market_snapshot, stochastic_vol

        try:
            m = market_snapshot.local_vol_market(w.ticker.upper(), w.rate, today)
            fit = stochastic_vol.calibrate(m)
        except (
            market_snapshot.MarketDataUnavailable,
            stochastic_vol.CalibrationUnavailable,
        ) as exc:
            raise UserError(str(exc)) from exc
        world = {
            "model": "heston",
            "spot": m.spot,
            "rate": w.rate,
            "dividend_yield": m.dividend,
            **{k: float(fit.heston[k]) for k in ("v0", "kappa", "theta", "xi", "rho")},
        }
        source = f"Heston calibrated on the stored option chain of {m.ticker}"
        snapshot = m.valuation_date
    elif w.model == "heston":
        world = {
            k: getattr(w, k)
            for k in (
                "model",
                "spot",
                "rate",
                "dividend_yield",
                "v0",
                "kappa",
                "theta",
                "xi",
                "rho",
            )
        }
        source, snapshot = "Heston, parameters typed", None
    else:
        world = {
            k: getattr(w, k)
            for k in ("model", "spot", "rate", "dividend_yield", "volatility")
        }
        source, snapshot = "Black-Scholes, parameters typed", None
    return world, HedgingData(
        source=source, snapshot=snapshot, fingerprints={"world": value_hash(world)}
    )


class UserError(ValueError):
    """A refusal to show as it is (missing data, a calibration that failed)."""

    user_facing = True


def _hedge_result(name: str, raw: dict, edges: np.ndarray) -> HedgeResult:
    explanation = raw["explanation"]
    counts, _ = np.histogram(raw["pnl"], bins=edges)
    return HedgeResult(
        hedge=name,
        label=LABELS[name],
        mean=engine._metric(raw["mean"]),
        standard_deviation=engine._metric(raw["standard_deviation"]),
        skewness=engine._number(raw["skewness"]),
        kurtosis=engine._number(raw["kurtosis"]),
        quantiles={k: engine._number(v) for k, v in raw["quantiles"].items()},
        step_error_autocorrelation=engine._number(raw["step_error_autocorrelation"]),
        discretisation_noise=engine._number(raw["discretisation_noise"]),
        gamma_integral=engine._metric(raw["gamma_integral"]),
        explanation={
            k: engine._metric(v) for k, v in explanation.items() if isinstance(v, dict)
        },
        unexplained_step_deviation=engine._number(
            explanation.get("unexplained_step_deviation")
        ),
        option_step_deviation=engine._number(explanation.get("option_step_deviation")),
        uncalibrated_steps=int(raw["uncalibrated_steps"]),
        convention=raw["convention"],
        histogram=[int(c) for c in counts],
    )


def run_hedging(req: HedgingRequest, task: Optional[Task] = None) -> HedgingResponse:
    """The run, a pure function of its request (the seeds are in it). With a
    task, progress is reported to it and its cancellation stops the engine."""
    started = time.perf_counter()
    report = task.report if task is not None else (lambda *_a, **_k: False)
    report(0.0, "Reading the world")
    world, data = _world(req, date.today())
    strike = req.strike * world["spot"]
    steps = (["fit"] if "minimum_variance" in req.hedges else []) + list(req.hedges)
    done = {"i": 0}

    def callbacks(label: str):
        i = done["i"]

        def progress(fraction: float) -> None:
            report((i + float(fraction)) / len(steps), label)

        return progress, (lambda: bool(task and task.cancel_requested))

    common = dict(
        strike=strike,
        maturity=req.maturity,
        rebalances=req.rebalances,
        paths=req.paths,
        is_call=req.is_call,
        threads=engine.THREADS,
    )
    coefficients = None
    raws: dict[str, dict] = {}
    try:
        if "minimum_variance" in req.hedges:
            progress, cancel = callbacks(
                "Fitting the minimum-variance delta on another seed"
            )
            # Fitted on another seed than the hedges are measured on
            # (contract §5): in-sample it would flatter itself.
            coefficients = qb.fit_minimum_variance_delta(
                world,
                seed=req.seed + 1_000_003,
                progress=progress,
                cancel=cancel,
                **common,
            )
            done["i"] += 1
        for name in req.hedges:
            progress, cancel = callbacks(f"Hedging with: {LABELS[name].lower()}")
            extra: dict[str, Any] = {}
            if name == "black_scholes_fixed":
                extra["hedge_volatility"] = req.hedge_volatility or (
                    world["volatility"]
                    if world["model"] == "black_scholes"
                    else math.sqrt(world["v0"])
                )
            if name == "minimum_variance":
                extra["coefficients"] = coefficients
            raws[name] = qb.hedging_backtest(
                world,
                hedge=name,
                seed=req.seed,  # every hedge on the same paths
                cost_rate=req.cost_rate,
                confidence=req.confidence,
                progress=progress,
                cancel=cancel,
                **common,
                **extra,
            )
            done["i"] += 1
    except qb.Cancelled as exc:
        raise Cancelled() from exc
    except qb.Error as exc:
        raise UserError(str(exc)) from exc

    # Common bins: from the lowest 0.5 % to the highest 99.5 % point of the hedges.
    lows = [float(np.percentile(r["pnl"], 0.5)) for r in raws.values()]
    highs = [float(np.percentile(r["pnl"], 99.5)) for r in raws.values()]
    lo, hi = min(lows), max(highs)
    if not hi > lo:
        lo, hi = lo - 1.0, hi + 1.0
    edges = np.linspace(lo, hi, HISTOGRAM_BINS + 1)
    first = next(iter(raws.values()))
    return HedgingResponse(
        world=world,
        strike=strike,
        maturity=req.maturity,
        is_call=req.is_call,
        premium=float(first["premium"]),
        hedges=[_hedge_result(n, raws[n], edges) for n in req.hedges],
        bins=[float(e) for e in edges],
        coefficients=(
            {k: float(v) for k, v in coefficients.items()} if coefficients else None
        ),
        data=data,
        engine=engine.build(),
        compute_ms=(time.perf_counter() - started) * 1e3,
    )
