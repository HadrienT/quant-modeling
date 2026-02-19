import os
import time

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from starlette.middleware.base import BaseHTTPMiddleware

from .logging_utils import configure_logging, get_logger
from .request_context import is_cache_hit, reset_cache_hit
from .routers.market import router as market_router
from .routers.pricing import router as pricing_router


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


app = FastAPI(title="quantModeling API", version="0.1.0")

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


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}


app.include_router(market_router)
app.include_router(pricing_router)

