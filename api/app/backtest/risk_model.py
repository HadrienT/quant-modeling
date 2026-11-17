"""The risk-model tab (blueprint/wp/24-backtest.md, lot B8): was a
portfolio's value at risk right?

Each day of the last year the VaR the model announced the evening before is
set against the day's hypothetical P&L (portfolio_risk.var_history: the
holdings of the day before held still, every instrument revalued with the
day's market). The tests are the wheel's (`var_backtest`): Kupiec's
unconditional coverage, Christoffersen's independence and conditional
coverage, and the Basel traffic light. Nothing is computed here but the
series handed to it.
"""

from __future__ import annotations

import time
from datetime import date
from typing import Literal, Optional

import numpy as np
import quantbacktest as qb
from pydantic import BaseModel, Field

from .. import portfolio_risk
from ..portfolio_ledger import migrate, validate
from ..portfolio_schemas import Portfolio
from ..tasks import Cancelled, Task
from ..valuation import value_hash
from . import engine
from . import schemas as s


class RiskModelRequest(BaseModel):
    portfolio: Portfolio
    end: Optional[date] = Field(None, description="Last day judged; default today.")
    days: int = Field(250, ge=60, le=500, description="Business days judged.")
    window: int = Field(
        portfolio_risk.VAR_WINDOW, ge=portfolio_risk.VAR_MIN_SCENARIOS, le=500
    )
    confidence: Literal[0.99, 0.975] = 0.99


class Test(BaseModel):
    statistic: Optional[float]
    p_value: Optional[float]
    degrees_of_freedom: Optional[float]


class TrafficLight(BaseModel):
    zone: str
    cumulative_probability: Optional[float]
    basel_plus_factor: Optional[float] = None
    frtb_multiplier: Optional[float] = None


class RiskModelData(BaseModel):
    fingerprints: dict[str, str]


class RiskModelResponse(BaseModel):
    base_currency: str
    confidence: float
    window: int
    dates: list[date]
    pnl: list[float] = Field(..., description="Hypothetical P&L of each day.")
    value_at_risk: list[float] = Field(
        ..., description="VaR announced the evening before, a positive loss."
    )
    exception_days: list[int]
    observations: int
    exceptions: int
    expected_exceptions: float
    unconditional_coverage: Test
    independence: Test
    conditional_coverage: Test
    transitions: list[list[int]]
    traffic_light: TrafficLight
    skipped: int
    data: RiskModelData
    engine: s.EngineBuild
    compute_ms: float


class UserError(ValueError):
    user_facing = True


def _test(raw: dict) -> Test:
    return Test(
        statistic=engine._number(raw.get("statistic")),
        p_value=engine._number(raw.get("p_value")),
        degrees_of_freedom=engine._number(raw.get("degrees_of_freedom")),
    )


def run_risk_model(
    req: RiskModelRequest, task: Optional[Task] = None
) -> RiskModelResponse:
    started = time.perf_counter()
    pf = migrate(req.portfolio)
    try:
        validate(pf)
    except ValueError as exc:
        raise UserError(str(exc)) from exc
    end = req.end or date.today()
    if task is not None:
        task.report(0.0, "Revaluing every position on each day of the two windows")

    def progress(f: float) -> bool:
        return task.report(0.95 * f) if task is not None else False

    try:
        h = portfolio_risk.var_history(
            pf, end, req.days, req.window, req.confidence, progress
        )
    except Cancelled:
        raise
    except ValueError as exc:
        raise UserError(str(exc)) from exc
    if len(h.pnl) < 20:
        raise UserError(
            f"only {len(h.pnl)} days have both a VaR and a P&L: too few to judge the model"
        )
    raw = qb.var_backtest(
        np.array(h.pnl), np.array(h.var), tail_probability=1.0 - req.confidence
    )
    light = raw["traffic_light"]
    return RiskModelResponse(
        base_currency=pf.base_currency,
        confidence=req.confidence,
        window=req.window,
        dates=h.dates,
        pnl=h.pnl,
        value_at_risk=h.var,
        exception_days=[int(x) for x in raw["exception_days"]],
        observations=int(raw["observations"]),
        exceptions=int(raw["exceptions"]),
        expected_exceptions=float(raw["expected_exceptions"]),
        unconditional_coverage=_test(raw["unconditional_coverage"]),
        independence=_test(raw["independence"]),
        conditional_coverage=_test(raw["conditional_coverage"]),
        transitions=[[int(v) for v in row] for row in raw["transitions"]],
        traffic_light=TrafficLight(
            zone=str(light["zone"]),
            cumulative_probability=engine._number(light.get("cumulative_probability")),
            basel_plus_factor=engine._number(light.get("basel_plus_factor")),
            frtb_multiplier=engine._number(light.get("frtb_multiplier")),
        ),
        skipped=h.skipped,
        data=RiskModelData(
            fingerprints={
                "portfolio": value_hash(pf.model_dump(mode="json")),
                "marks": h.values_fingerprint,
            }
        ),
        engine=engine.build(),
        compute_ms=(time.perf_counter() - started) * 1e3,
    )
