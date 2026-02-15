import json
import logging
from pathlib import Path


class JsonFormatter(logging.Formatter):
    _standard = {
        "args",
        "asctime",
        "created",
        "exc_info",
        "exc_text",
        "filename",
        "funcName",
        "levelname",
        "levelno",
        "lineno",
        "module",
        "msecs",
        "message",
        "msg",
        "name",
        "pathname",
        "process",
        "processName",
        "relativeCreated",
        "stack_info",
        "thread",
        "threadName",
    }

    @staticmethod
    def _round_floats(value):
        if isinstance(value, float):
            return round(value, 3)
        if isinstance(value, dict):
            return {key: JsonFormatter._round_floats(val) for key, val in value.items()}
        if isinstance(value, (list, tuple)):
            return [JsonFormatter._round_floats(val) for val in value]
        return value

    def format(self, record: logging.LogRecord) -> str:
        extras = {
            key: value
            for key, value in record.__dict__.items()
            if key not in self._standard
        }
        payload = {
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
            "time": self.formatTime(record, "%Y-%m-%dT%H:%M:%S"),
        }
        if extras:
            payload["extra"] = self._round_floats(extras)
        return json.dumps(payload, ensure_ascii=True)


def _configure_logging() -> logging.Logger:
    log_dir = Path("/tmp/quantmodeling")
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / "api.jsonl"

    handler = logging.FileHandler(log_path, encoding="utf-8")
    handler.setFormatter(JsonFormatter())

    api_logger = logging.getLogger("quantmodeling.api")
    api_logger.setLevel(logging.INFO)
    api_logger.handlers.clear()
    api_logger.addHandler(handler)
    api_logger.propagate = False
    return api_logger

from fastapi import FastAPI

from .pricing_service import price_asian, price_vanilla
from .schemas import AsianRequest, PricingResponse, VanillaRequest


logger = _configure_logging()

app = FastAPI(title="quantModeling API", version="0.1.0")


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}


@app.post("/price/vanilla", response_model=PricingResponse)
def price_vanilla_endpoint(req: VanillaRequest) -> PricingResponse:
    logger.info(
        "price_vanilla request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "engine": req.engine,
            "n_paths": req.n_paths,
            "seed": req.seed,
            "mc_epsilon": req.mc_epsilon,
        },
    )
    resp = price_vanilla(req)
    logger.info(
        "price_vanilla response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp


@app.post("/price/asian", response_model=PricingResponse)
def price_asian_endpoint(req: AsianRequest) -> PricingResponse:
    logger.info(
        "price_asian request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "average_type": req.average_type,
            "engine": req.engine,
            "n_paths": req.n_paths,
            "seed": req.seed,
            "mc_epsilon": req.mc_epsilon,
        },
    )
    resp = price_asian(req)
    logger.info(
        "price_asian response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp
