"""Router — the /rates page: multi-curve bootstrap, swap, swaption models and
a calibrated Hull-White with its Bermudan.

GET  /api/rates/example   the illustrative quotes the page starts from
POST /api/rates/analyse   curves, swap, Hull-White calibration, swaption

The computation and the methodology live in `rates_derivatives.py`.
"""

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from .. import rates_derivatives
from ..rates_derivatives_schemas import (
    RatesAnalysisRequest,
    RatesAnalysisResponse,
    RatesExampleResponse,
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


@router.post("/api/rates/analyse", response_model=RatesAnalysisResponse)
async def rates_analyse(req: RatesAnalysisRequest) -> RatesAnalysisResponse:
    try:
        # A calibration is ~1 s of C++: off the event loop.
        return await run_in_threadpool(rates_derivatives.analyse, req)
    except RuntimeError as exc:
        # InvalidInput from the C++: quotes no curve can reprice, a swap
        # before its expiry... the user's input, not a server fault.
        raise HTTPException(status_code=422, detail=str(exc)) from exc
