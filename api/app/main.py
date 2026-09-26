import logging
import os

from fastapi import FastAPI, HTTPException, Request
from fastapi.encoders import jsonable_encoder
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.routing import APIRoute
from pydantic import BaseModel

from .audit.middleware import AuditMiddleware
from .audit.sinks import close_sink, get_sink
from .logging_utils import LOGGER_NAME, configure_logging
from .routers.market import router as market_router
from .routers.pricing import router as pricing_router
from .routers.local_vol_pricing import router as local_vol_router
from .routers.simulation import router as simulation_router
from .routers.portfolio import router as portfolio_router
from .routers.portfolio_valuation import router as portfolio_valuation_router
from .routers.auth import router as auth_router
from .routers.backtest import router as backtest_router
from .routers.assistant import router as assistant_router
from .routers.admin import router as admin_router
from .telemetry import setup_telemetry, shutdown_telemetry

configure_logging()


def _operation_id(route: APIRoute) -> str:
    """Readable operationIds → readable generated hook names (blueprint WP 02 §6.2)."""
    name = route.name
    for suffix in ("_endpoint", "_route"):
        if name.endswith(suffix):
            name = name[: -len(suffix)]
    return name


app = FastAPI(
    title="quantModeling API",
    version="0.1.0",
    generate_unique_id_function=_operation_id,
)


class ApiError(BaseModel):
    """Single error shape for the whole API (blueprint WP 02 §6.4)."""

    code: str
    message: str
    detail: object | None = None


@app.exception_handler(HTTPException)
async def _http_exception_handler(_: Request, exc: HTTPException) -> JSONResponse:
    codes = {
        400: "bad_request",
        401: "unauthorized",
        403: "forbidden",
        404: "not_found",
        409: "conflict",
        422: "unprocessable",
        429: "rate_limited",
    }
    return JSONResponse(
        status_code=exc.status_code,
        content=ApiError(
            code=codes.get(exc.status_code, "error"),
            message=str(exc.detail) if exc.detail else "Request failed",
        ).model_dump(),
        headers=getattr(exc, "headers", None),
    )


@app.exception_handler(RequestValidationError)
async def _validation_exception_handler(
    _: Request, exc: RequestValidationError
) -> JSONResponse:
    return JSONResponse(
        status_code=422,
        content=ApiError(
            code="unprocessable",
            message="One or more parameters are invalid.",
            # A custom validator that raises ValueError leaves the exception
            # itself in `ctx.error`, which json cannot serialise: stringify it
            # rather than turn a 422 into a 500.
            detail=jsonable_encoder(exc.errors(), custom_encoder={Exception: str}),
        ).model_dump(),
    )


cors_origins = [
    origin.strip()
    for origin in os.getenv("CORS_ALLOW_ORIGINS", "").split(",")
    if origin.strip()
]
if not cors_origins:
    cors_origins = ["*"]

app.add_middleware(
    CORSMiddleware,
    allow_origins=cors_origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.add_middleware(AuditMiddleware)
# Outermost, so the audit middleware already runs inside the request's span and
# every audit event carries its trace id (blueprint WP 18d).
setup_telemetry(app)


class HealthResponse(BaseModel):
    status: str
    version: str


@app.get("/health", response_model=HealthResponse)
def health() -> HealthResponse:
    return HealthResponse(status="ok", version=os.getenv("COMMIT_SHA", "dev"))


@app.on_event("startup")
def _start_audit_sink() -> None:
    # Built at startup rather than on the first event: the Kafka producer's
    # thread starts republishing a spool left by a previous run straight away.
    get_sink()


@app.on_event("startup")
def _warm_up_gpu() -> None:
    # The first CUDA call of a process creates the context and loads the
    # kernels (~0.3 s). Paid here, so that the compute time shown for the first
    # GPU pricing is the pricing's own (blueprint/wp/19-gpu.md §8). A failure
    # only means the GPU is unusable: the API keeps pricing on the CPU.
    import quantmodeling as qm

    if not qm.gpu_devices():
        return
    try:
        qm.gpu_warm_up()
        logging.getLogger(LOGGER_NAME).info("gpu warmed up: %s", qm.gpu_devices()[0])
    except Exception:  # noqa: BLE001
        logging.getLogger(LOGGER_NAME).exception("gpu warm-up failed")


@app.on_event("shutdown")
def _flush_audit_and_telemetry() -> None:
    # Queued audit events are flushed to Kafka, or spooled (blueprint WP 18b).
    close_sink()
    shutdown_telemetry()


@app.on_event("shutdown")
def _close_db_pool() -> None:
    from .db import close_pool

    try:
        close_pool()
    except Exception:  # noqa: BLE001
        pass


app.include_router(market_router)
app.include_router(pricing_router)
app.include_router(local_vol_router)
app.include_router(simulation_router)
app.include_router(portfolio_router)
app.include_router(portfolio_valuation_router)
app.include_router(auth_router)
app.include_router(backtest_router)
app.include_router(assistant_router)
app.include_router(admin_router)
