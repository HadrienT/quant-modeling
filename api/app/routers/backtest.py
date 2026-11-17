"""Router — backtests (blueprint/wp/24-backtest.md, lot B8).

GET  /api/backtest/universe     what can be backtested, and on which engine
POST /api/backtest/strategy     an allocation strategy, with its comparison
                                points, its metrics and their intervals
POST /api/backtest/validation   a grid of configurations against overfitting
POST /api/backtest/hedging      an option delta-hedged in a simulated world:
                                a task (minutes in a Heston world), whose id
                                is returned at once
GET  /api/backtest/tasks/{id}   a task's progress, then its result
DELETE /api/backtest/tasks/{id} cancel it

It validates, reads, calls the `quantbacktest` wheel and returns: the engine
lives in the quant-backtest repository and nothing is computed here
(`backtest/engine.py` says what little is done to a number). Every run emits a
`backtest.run` audit event that `POST /api/admin/replay/{id}` replays.
"""

from __future__ import annotations

from typing import Any, Callable, Literal, Optional

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from ..audit import emit
from .. import tasks
from ..backtest import engine, market, record, universe
from ..backtest.hedging import HedgingRequest, HedgingResponse, run_hedging
from ..backtest.schemas import (
    StrategyRequest,
    StrategyResponse,
    UniverseResponse,
    ValidationRequest,
    ValidationResponse,
)

router = APIRouter(prefix="/api/backtest", tags=["backtest"])

_ERRORS = {
    422: {"description": "A datum is missing, or the engine refuses the request."},
    503: {"description": "The market data store is unreachable."},
}


async def _guarded(call: Callable[..., Any], *args: Any) -> Any:
    try:
        # The engine releases the GIL: off the event loop.
        return await run_in_threadpool(call, *args)
    except (market.MissingData, engine.Error) as exc:
        # Both carry an English message meant to be shown as it is.
        raise HTTPException(status_code=422, detail=str(exc)) from exc


@router.get("/universe", response_model=UniverseResponse, responses=_ERRORS)
async def backtest_universe() -> UniverseResponse:
    return await _guarded(universe.universe)


@router.post("/strategy", response_model=StrategyResponse, responses=_ERRORS)
async def backtest_strategy(req: StrategyRequest) -> StrategyResponse:
    response = await _guarded(engine.run_strategy, req)
    emit(record.EVENT_TYPE, record.payload("strategy", req, response))
    return response


@router.post("/validation", response_model=ValidationResponse, responses=_ERRORS)
async def backtest_validation(req: ValidationRequest) -> ValidationResponse:
    response = await _guarded(engine.run_validation, req)
    emit(record.EVENT_TYPE, record.payload("validation", req, response))
    return response


# ── Hedging: a task ──────────────────────────────────────────────────────────


class TaskStatus(BaseModel):
    id: str
    kind: str
    status: Literal["queued", "running", "done", "failed", "cancelled"]
    progress: float
    step: str
    #: Tasks running or waiting before this one (0: it runs).
    ahead: int
    error: Optional[str] = None
    hedging: Optional[HedgingResponse] = None


def _status(task: tasks.Task) -> TaskStatus:
    return TaskStatus(
        id=task.id,
        kind=task.kind,
        status=task.status,
        progress=task.progress,
        step=task.step,
        ahead=tasks.position(task) if task.status == "queued" else 0,
        error=task.error,
        hedging=task.result if task.kind == "hedging" else None,
    )


@router.post(
    "/hedging",
    response_model=TaskStatus,
    status_code=202,
    responses={429: {"description": "Too many backtests queued."}},
)
async def backtest_hedging(req: HedgingRequest) -> TaskStatus:
    def record_it(response: HedgingResponse) -> None:
        emit(record.EVENT_TYPE, record.payload("hedging", req, response))

    try:
        task = tasks.submit("hedging", lambda t: run_hedging(req, t), on_done=record_it)
    except tasks.QueueFull as exc:
        raise HTTPException(status_code=429, detail=str(exc)) from exc
    return _status(task)


def _task(task_id: str) -> tasks.Task:
    task = tasks.get(task_id)
    if task is None:
        raise HTTPException(
            status_code=404,
            detail="No such task: it finished more than an hour ago, or the "
            "server restarted. Run it again.",
        )
    return task


@router.get("/tasks/{task_id}", response_model=TaskStatus)
async def backtest_task(task_id: str) -> TaskStatus:
    return _status(_task(task_id))


@router.delete("/tasks/{task_id}", response_model=TaskStatus)
async def backtest_cancel(task_id: str) -> TaskStatus:
    _task(task_id)
    return _status(tasks.cancel(task_id))
