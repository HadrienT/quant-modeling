import json
import logging
import os
from pathlib import Path

LOGGER_NAME = "quantmodeling.api"


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


def configure_logging() -> logging.Logger:
    log_dir = Path(os.getenv("LOG_DIR", "/tmp/quantmodeling"))
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / "api.jsonl"

    handler = logging.FileHandler(log_path, encoding="utf-8")
    handler.setFormatter(JsonFormatter())

    api_logger = logging.getLogger(LOGGER_NAME)
    api_logger.setLevel(logging.INFO)
    api_logger.handlers.clear()
    api_logger.addHandler(handler)
    api_logger.propagate = False
    return api_logger


def get_logger() -> logging.Logger:
    return logging.getLogger(LOGGER_NAME)
