"""Router — backtests (blueprint/wp/24-backtest.md, lot B8).

GET  /api/backtest/universe     what can be backtested, and on which engine
POST /api/backtest/strategy     an allocation strategy, with its comparison
                                points, its metrics and their intervals
POST /api/backtest/validation   a grid of configurations against overfitting

It validates, reads, calls the `quantbacktest` wheel and returns: the engine
lives in the quant-backtest repository and nothing is computed here
(`backtest/engine.py` says what little is done to a number). Every run emits a
`backtest.run` audit event that `POST /api/admin/replay/{id}` replays.
"""

from __future__ import annotations

from typing import Any, Callable

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from .. import db
from ..audit import emit
from ..backtest import engine, market, record, universe
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
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
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
