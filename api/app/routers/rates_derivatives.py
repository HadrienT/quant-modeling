"""Router — rates derivatives: the quote sets and the curves built from them.

GET  /api/rates/quotes/{set_id}   a quote set: USD SOFR from traded swaps and
                                  swaptions, or the illustrative EUR one
POST /api/rates/curves            the OIS and index curves of a set of quotes

The swap and the swaption priced on those quotes are pricing endpoints
(`POST /price/rates/swap`, `/price/rates/swaption`, routers/pricing.py), so
each valuation is recorded and can be replayed. The computation and the
methodology live in `rates_derivatives.py`.
"""

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from .. import db, rates_derivatives
from ..rates_derivatives_schemas import (
    QuoteSetId,
    RatesCurveQuotes,
    RatesCurvesResponse,
    RatesQuoteSetResponse,
)

router = APIRouter()


@router.get("/api/rates/quotes/{set_id}", response_model=RatesQuoteSetResponse)
def rates_quote_set(set_id: QuoteSetId) -> RatesQuoteSetResponse:
    try:
        return rates_derivatives.quote_set(set_id)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    except rates_derivatives.RatesMarketUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc


@router.post("/api/rates/curves", response_model=RatesCurvesResponse)
async def rates_curves(quotes: RatesCurveQuotes) -> RatesCurvesResponse:
    try:
        built = await run_in_threadpool(rates_derivatives.curves, quotes)
    except RuntimeError as exc:
        # InvalidInput from the C++: quotes no curve can reprice. The user's
        # input, not a server fault.
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return RatesCurvesResponse(
        curves=built, methodology=rates_derivatives.curves_methodology()
    )
