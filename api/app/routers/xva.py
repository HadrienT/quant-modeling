"""Router — xVA of a netting set (blueprint/wp/23-xva.md, lot X3).

GET  /api/xva/portfolios    the teaching portfolios and the ratings
POST /api/xva/netting-set   exposure, CVA / DVA and each trade's share
POST /api/xva/portfolio     the same for a netting set of a portfolio's ledger
POST /api/xva/sensitivities             sensitivities by adjoint differentiation
POST /api/xva/portfolio/sensitivities   the same for a netting set of a ledger

The computation and the methodology live in `xva.py`. Every run emits an
`xva.valuation` audit event with its request, seed, model and market inputs
(`xva_record.py`, which also replays it).
"""

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from .. import db, portfolio_xva, xva, xva_record
from ..audit import emit
from ..xva_schemas import (
    WrongWayScenario,
    XvaPortfolioInfo,
    XvaPortfoliosResponse,
    XvaRequest,
    XvaResponse,
    XvaSensitivitiesRequest,
    XvaSensitivitiesResponse,
)

router = APIRouter()


@router.get("/api/xva/portfolios", response_model=XvaPortfoliosResponse)
def xva_portfolios() -> XvaPortfoliosResponse:
    return XvaPortfoliosResponse(
        portfolios=[
            XvaPortfolioInfo(id=pid, label=label, lesson=lesson)
            for pid, label, lesson in xva.portfolios()
        ],
        ratings=xva.RATINGS,
        wrong_way_scenarios=[
            WrongWayScenario(id=i, label=label, wrong_way_risk=b, explanation=text)
            for i, label, b, text in xva.wrong_way_scenarios()
        ],
    )


@router.post("/api/xva/netting-set", response_model=XvaResponse)
async def xva_netting_set(req: XvaRequest) -> XvaResponse:
    try:
        # A few seconds of C++ on several threads: off the event loop.
        response = await run_in_threadpool(xva.compute, req)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    except xva.XvaUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except xva.XvaInputError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except RuntimeError as exc:
        # InvalidInput from the C++: an input no curve or model accepts.
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    emit(xva_record.EVENT_TYPE, xva_record.payload(req, response))
    return response


@router.post("/api/xva/portfolio", response_model=XvaResponse)
async def xva_portfolio(req: portfolio_xva.PortfolioXvaRequest) -> XvaResponse:
    """Exposure and xVA of one netting set of a portfolio: the trades held
    under it today, the counterparty's rating and the collateral agreement
    of the ledger, on the market of the xVA page."""
    try:
        run, response = await run_in_threadpool(portfolio_xva.compute, req)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    except xva.XvaUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except xva.XvaInputError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    emit(xva_record.EVENT_TYPE, xva_record.payload(run, response, trades=True))
    return response


@router.post("/api/xva/sensitivities", response_model=XvaSensitivitiesResponse)
async def xva_sensitivities(req: XvaSensitivitiesRequest) -> XvaSensitivitiesResponse:
    """The sensitivities of the adjustments to every market quote, by adjoint
    differentiation, and SA-CVA from them."""
    try:
        return await run_in_threadpool(xva.compute_sensitivities, req)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    except xva.XvaUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except xva.XvaInputError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc


@router.post(
    "/api/xva/portfolio/sensitivities", response_model=XvaSensitivitiesResponse
)
async def xva_portfolio_sensitivities(
    req: portfolio_xva.PortfolioXvaSensitivitiesRequest,
) -> XvaSensitivitiesResponse:
    """The sensitivities of the adjustments of one netting set of a
    portfolio to every market quote, by adjoint differentiation."""
    try:
        return await run_in_threadpool(portfolio_xva.compute_sensitivities, req)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    except xva.XvaUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except xva.XvaInputError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
