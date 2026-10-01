"""Router — the /rates page: multi-curve bootstrap, swap, swaption models and
a calibrated Hull-White with its Bermudan.

GET  /api/rates/example   the illustrative quotes the page starts from
GET  /api/rates/market    USD SOFR quotes built from traded swaps and swaptions
POST /api/rates/analyse   curves, swap, Hull-White calibration, swaption

The computation and the methodology live in `rates_derivatives.py`.
"""

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from .. import db, rates_derivatives
from ..rates_derivatives_schemas import (
    RatesAnalysisRequest,
    RatesAnalysisResponse,
    RatesExampleResponse,
    RatesMarketResponse,
)

router = APIRouter()


@router.get("/api/rates/example", response_model=RatesExampleResponse)
def rates_example() -> RatesExampleResponse:
    e = rates_derivatives.example_request()
    return RatesExampleResponse(
        currency=e["currency"],
        label=e["label"],
        request=RatesAnalysisRequest.model_validate(e),
    )


@router.get("/api/rates/market", response_model=RatesMarketResponse)
def rates_market() -> RatesMarketResponse:
    try:
        m = rates_derivatives.market_request()
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    except rates_derivatives.RatesMarketUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    return RatesMarketResponse(
        currency=m["currency"],
        label=m["label"],
        as_of=m["as_of"],
        window_start=m["window_start"],
        request=RatesAnalysisRequest.model_validate(m),
        swap_rates=m["swap_rates"],
        swaption_vols=m["market_vols"],
        trades_used=m["trades_used"],
        rejected=m["rejected"],
    )


@router.post("/api/rates/analyse", response_model=RatesAnalysisResponse)
async def rates_analyse(req: RatesAnalysisRequest) -> RatesAnalysisResponse:
    try:
        # A calibration is ~1 s of C++: off the event loop.
        return await run_in_threadpool(rates_derivatives.analyse, req)
    except RuntimeError as exc:
        # InvalidInput from the C++: quotes no curve can reprice, a swap
        # before its expiry... the user's input, not a server fault.
        raise HTTPException(status_code=422, detail=str(exc)) from exc
