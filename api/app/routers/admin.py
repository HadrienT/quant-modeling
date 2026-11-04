"""Router — administration. Today: the replay of a recorded valuation
(blueprint WP 18e, `replay.py`), of a recorded xVA (`xva_record.py`) or of a
recorded backtest (`backtest/record.py`)."""

from __future__ import annotations

from typing import Union

from fastapi import APIRouter, Depends, HTTPException
from starlette.concurrency import run_in_threadpool

from ..audit.store import AuditStoreUnavailable, get_store
from ..auth import require_admin
from ..backtest import record as backtest_record
from .. import db, xva, xva_record
from ..replay import NotAValuation, ReplayResponse, replay

Replayed = Union[
    ReplayResponse,
    xva_record.XvaReplayResponse,
    backtest_record.BacktestReplayResponse,
]

router = APIRouter(prefix="/api/admin", tags=["admin"])


@router.post(
    "/replay/{event_id}",
    response_model=Replayed,
    responses={
        403: {"description": "Not an administrator."},
        404: {"description": "No such event in the audit database."},
        422: {"description": "The event is not a replayable valuation."},
        503: {
            "description": "The audit database is not configured or unreachable, "
            "or the market data an xVA needs is missing."
        },
    },
)
async def replay_valuation_endpoint(
    event_id: str, _admin: str = Depends(require_admin)
) -> Replayed:
    """Re-runs the valuation, the xVA or the backtest recorded under `event_id`
    and compares: `reproduced`, `drifted_inputs` (which datum), `drifted_code`
    (from which build to which) or `not_reproduced`."""
    try:
        event = await run_in_threadpool(get_store().get, event_id)
    except AuditStoreUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    if event is None:
        raise HTTPException(status_code=404, detail=f"no event {event_id}")
    run = {
        backtest_record.EVENT_TYPE: backtest_record.replay,
        xva_record.EVENT_TYPE: xva_record.replay,
    }.get(event.get("type"), replay)
    try:
        return await run_in_threadpool(run, event)
    except (db.StoreUnavailable, xva.XvaUnavailable) as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except xva.XvaInputError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except (backtest_record.market.MissingData, backtest_record.engine.Error) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except NotAValuation as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
