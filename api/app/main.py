import os
import time

from fastapi import FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.routing import APIRoute
from pydantic import BaseModel
from starlette.middleware.base import BaseHTTPMiddleware

from .logging_utils import configure_logging, get_logger
from .request_context import is_cache_hit, reset_cache_hit
from .routers.market import router as market_router
from .routers.pricing import router as pricing_router
from .routers.local_vol_pricing import router as local_vol_router
from .routers.portfolio import router as portfolio_router
from .routers.auth import router as auth_router
from .routers.backtest import router as backtest_router


configure_logging()
logger = get_logger()


class CacheLoggingMiddleware(BaseHTTPMiddleware):
    """Middleware to log cache hits in access logs."""
    
    async def dispatch(self, request: Request, call_next):
        # Reset cache hit status for this request
        reset_cache_hit()
        
        start_time = time.time()
        response = await call_next(request)
        process_time = time.time() - start_time
        
        # Check if this request hit cache
        cache_indicator = " (cache)" if is_cache_hit() else ""
        
        # Log the request with cache indicator
        logger.info(
            f"{request.client.host if request.client else 'unknown'}:{request.client.port if request.client else 0} - "
            f'"{request.method} {request.url.path}{"?" + str(request.url.query) if request.url.query else ""} '
            f'HTTP/{request.scope.get("http_version", "1.1")}" {response.status_code}{cache_indicator} '
            f"({process_time:.3f}s)"
        )
        
        return response


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
    codes = {400: "bad_request", 401: "unauthorized", 403: "forbidden",
             404: "not_found", 409: "conflict", 422: "unprocessable", 429: "rate_limited"}
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
            detail=exc.errors(),
        ).model_dump(),
    )

cors_origins = [origin.strip() for origin in os.getenv("CORS_ALLOW_ORIGINS", "").split(",") if origin.strip()]
if not cors_origins:
    cors_origins = ["*"]

app.add_middleware(
    CORSMiddleware,
    allow_origins=cors_origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.add_middleware(CacheLoggingMiddleware)


class HealthResponse(BaseModel):
    status: str
    version: str


@app.get("/health", response_model=HealthResponse)
def health() -> HealthResponse:
    return HealthResponse(status="ok", version=os.getenv("COMMIT_SHA", "dev"))


app.include_router(market_router)
app.include_router(pricing_router)
app.include_router(local_vol_router)
app.include_router(portfolio_router)
app.include_router(auth_router)
app.include_router(backtest_router)

