"""Router — portfolio valuation (MTM, daily P&L, transactions).

Stateless: every endpoint takes the portfolio (its ledger) in the body, so the
same code values an anonymous portfolio kept in the browser and a signed-in
one stored on the server. The computation and the methodology text live in
`portfolio_valuation.py`; the accounting in `portfolio_ledger.py`.

POST /api/portfolio-valuation/snapshot   positions marked to market at a date
POST /api/portfolio-valuation/history    daily P&L over a window
POST /api/portfolio-valuation/migrate    a position-based portfolio → ledger
GET  /api/portfolio-valuation/close      a ticker's close on a date (trade form)
GET  /api/portfolio-valuation/demos      read-only demo portfolios
"""

from datetime import date, timedelta
from typing import Dict, List, Literal, Optional

from fastapi import APIRouter, HTTPException, Query
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool

from .. import db
from .. import portfolio_demos
from .. import portfolio_valuation as pv
from ..logging_utils import get_logger
from ..portfolio_ledger import migrate, validate
from ..portfolio_schemas import Portfolio
from ..schemas import MethodologySection

router = APIRouter(prefix="/api/portfolio-valuation", tags=["portfolios"])
logger = get_logger()

#: A valuation reprices every derivative for every day of a window.
MAX_INSTRUMENTS = 200
MAX_TRADES = 5000

WINDOWS = {"1M": 31, "3M": 92, "6M": 183, "1Y": 366, "ALL": 36500}


class InputView(BaseModel):
    name: str
    status: Literal["observed", "stale", "proxied", "default"]
    value: Optional[float]
    as_of: Optional[date]


class ModelParamView(BaseModel):
    name: str
    value: Optional[float] = None
    text: Optional[str] = None
    status: Literal["observed", "calibrated", "stale", "proxied", "contract", "default"]
    source: str = ""


class ModelView(BaseModel):
    """The model a derivative's mark was priced with (portfolio_models.py)."""

    model: str
    engine: str
    why: str
    params: List[ModelParamView]
    std_error: Optional[float] = Field(
        None, description="Monte-Carlo standard error of the unit mark."
    )


class PositionMark(BaseModel):
    instrument_id: str
    label: str
    kind: Literal["equity", "derivative"]
    currency: str
    quantity: float
    average_cost: float
    mark: Optional[float]
    market_value: Optional[float]
    market_value_base: Optional[float]
    unrealised: Optional[float]
    unrealised_base: Optional[float]
    realised_base: float
    fees_base: float
    day_pnl_base: Optional[float]
    fx_rate: Optional[float] = Field(
        description="Base units per instrument-currency unit."
    )
    inputs: List[InputView]
    greeks: Dict[str, Optional[float]]
    note: Optional[str]
    model: Optional[ModelView] = None


class SnapshotRequest(BaseModel):
    portfolio: Portfolio
    as_of: Optional[date] = None


class SnapshotResponse(BaseModel):
    as_of: date
    base_currency: str
    positions: List[PositionMark]
    market_value: float
    unrealised: float
    realised: float
    fees: float
    total_pnl: float
    day_pnl: Optional[float]
    warnings: List[str]
    methodology: List[MethodologySection]


class HistoryRequest(BaseModel):
    portfolio: Portfolio
    window: Literal["1M", "3M", "6M", "1Y", "ALL"] = "3M"
    end: Optional[date] = None


class HistoryPointView(BaseModel):
    date: date
    market_value: Optional[float]
    daily_pnl: Optional[float]
    cumulative_pnl: Optional[float]


class HistoryResponse(BaseModel):
    base_currency: str
    points: List[HistoryPointView]
    warnings: List[str]


class MigrateRequest(BaseModel):
    portfolio: Portfolio


class CloseResponse(BaseModel):
    ticker: str
    date: date
    close: float
    currency: str


def _checked(pf: Portfolio) -> Portfolio:
    pf = migrate(pf)
    if len(pf.instruments) > MAX_INSTRUMENTS or len(pf.transactions) > MAX_TRADES:
        raise HTTPException(
            status_code=422,
            detail=f"at most {MAX_INSTRUMENTS} instruments and {MAX_TRADES} trades",
        )
    try:
        validate(pf)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return pf


@router.post("/snapshot", response_model=SnapshotResponse)
async def portfolio_snapshot(req: SnapshotRequest) -> SnapshotResponse:
    pf = _checked(req.portfolio)
    as_of = req.as_of or date.today()
    try:
        s = await run_in_threadpool(pv.snapshot, pf, as_of)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    return SnapshotResponse(
        as_of=s.as_of,
        base_currency=s.base_currency,
        positions=[
            PositionMark(
                **{k: v for k, v in vars(p).items() if k not in ("inputs", "model")},
                inputs=[InputView(**vars(i)) for i in p.inputs],
                model=(
                    ModelView(
                        **{**vars(p.model), "params": [vars(x) for x in p.model.params]}
                    )
                    if p.model
                    else None
                ),
            )
            for p in s.positions
        ],
        market_value=s.market_value,
        unrealised=s.unrealised,
        realised=s.realised,
        fees=s.fees,
        total_pnl=s.total_pnl,
        day_pnl=s.day_pnl,
        warnings=s.warnings,
        methodology=[
            MethodologySection(title=t, paragraphs=p) for t, p in pv.METHODOLOGY
        ],
    )


@router.post("/history", response_model=HistoryResponse)
async def portfolio_history(req: HistoryRequest) -> HistoryResponse:
    pf = _checked(req.portfolio)
    end = req.end or date.today()
    start = end - timedelta(days=WINDOWS[req.window])
    try:
        points, warnings = await run_in_threadpool(pv.history, pf, start, end)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    return HistoryResponse(
        base_currency=pf.base_currency,
        points=[HistoryPointView(**vars(p)) for p in points],
        warnings=warnings,
    )


@router.post("/migrate", response_model=Portfolio)
async def portfolio_migrate(req: MigrateRequest) -> Portfolio:
    return migrate(req.portfolio)


@router.get("/close", response_model=CloseResponse)
async def ticker_close(
    ticker: str = Query(..., min_length=1, max_length=16),
    on: date = Query(..., alias="date"),
) -> CloseResponse:
    """The close on or before a date — the default price of a trade form."""
    ticker = ticker.strip().upper()
    try:
        rows = await run_in_threadpool(
            db.price_history, ticker, on - timedelta(days=14)
        )
        ccy = await run_in_threadpool(db.ticker_currency, ticker)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    rows = [r for r in rows if r[0] <= on]
    if not rows:
        raise HTTPException(
            status_code=404, detail=f"no close for {ticker} on or before {on}"
        )
    d, close = rows[-1]
    return CloseResponse(ticker=ticker, date=d, close=close, currency=ccy or "USD")


class DemoPortfolio(BaseModel):
    portfolio: Portfolio
    description: str


@router.get("/demos", response_model=List[DemoPortfolio])
async def portfolio_demos_list() -> List[DemoPortfolio]:
    """Demo ledgers, each trade priced from the stored market on its date
    (portfolio_demos.py). Built once a day; a demo whose data is missing is
    left out and logged."""
    try:
        built = await run_in_threadpool(portfolio_demos.demos)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    out = []
    for demo, pf, error in built:
        if pf is None:
            logger.warning("demo portfolio %s unavailable: %s", demo.id, error)
            continue
        out.append(DemoPortfolio(portfolio=pf, description=demo.description))
    return out
