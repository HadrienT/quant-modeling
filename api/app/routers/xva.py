"""Router — xVA of a netting set (blueprint/wp/23-xva.md, lot X3).

GET  /api/xva/portfolios    the teaching portfolios and the ratings
POST /api/xva/netting-set   exposure, CVA / DVA and each trade's share
POST /api/xva/portfolio     the same for a netting set of a portfolio's ledger
POST /api/xva/sensitivities             sensitivities by adjoint differentiation
POST /api/xva/portfolio/sensitivities   the same for a netting set of a ledger

The computation and the methodology live in `xva.py`. Every run emits an
`xva.valuation` audit event with its request, seed, model and market inputs.
"""

import os

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from .. import db, portfolio_xva, valuation, xva
from ..audit import emit
from ..audit.envelope import lib_build_sha
from ..audit.payloads import (
    CodeVersion,
    EngineSpec,
    MarketInput,
    MarketInputStatus,
    ModelSpec,
    ValuationTiming,
    XvaResult,
    XvaValuationPayload,
)
from ..request_context import current_ip_hash
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


def _payload(
    req: XvaRequest, response: XvaResponse, trades: bool = False
) -> XvaValuationPayload:
    request = req.model_dump(mode="json")
    if trades:
        # A netting set of a portfolio is not named by its id: its trades
        # are part of what was asked. The portfolio itself is not recorded.
        request["trades"] = [
            t.model_dump(
                mode="json",
                include={
                    "kind", "currency", "quantity", "payer", "notional",
                    "fixed_rate", "foreign_rate", "start", "tenor", "maturity",
                },
            )
            for t in response.trades
        ]  # fmt: skip
    hw = response.market.hull_white
    return XvaValuationPayload(
        request=request,
        request_hash=valuation.value_hash(request),
        model=ModelSpec(
            name="hull_white_one_factor",
            params={"mean_reversion": hw.mean_reversion, "sigma": hw.sigma},
        ),
        engine=EngineSpec(
            name="hull_white_exposure",
            n_paths=response.paths,
            seed=response.seed,
            scheme="exact",
            device=response.device,
        ),
        market_inputs=[
            MarketInput(
                name=name,
                source=source,
                as_of=as_of,
                status=MarketInputStatus.OBSERVED,
                value_hash=valuation.value_hash(value),
            )
            for name, source, as_of, value in xva.market_inputs(response)
        ],
        result=XvaResult(
            cva=response.adjustments.cva.value,
            cva_std_error=response.adjustments.cva.error,
            dva=response.adjustments.dva.value,
            dva_std_error=response.adjustments.dva.error,
            epe=response.exposure.epe,
            peak_pfe=max(response.exposure.pfe),
        ),
        timing=ValuationTiming(duration_ms=response.compute_ms),
        code=CodeVersion(
            api_sha=os.getenv("COMMIT_SHA", "dev"), lib_build=lib_build_sha()
        ),
        ip_hash=current_ip_hash(),
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
    emit("xva.valuation", _payload(req, response))
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
    emit("xva.valuation", _payload(run, response, trades=True))
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
