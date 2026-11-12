"""Stored calibrations: the digest that keys them and their JSON round trip.

The rough Bergomi and stochastic-vol calibrations are slow, so each one is
stored under a digest of everything it read and reloaded on the next start.
A stored entry is a dataclass with a ``snapshot`` date and some tuple fields,
which JSON keeps as an ISO string and lists.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import logging
from datetime import date
from typing import Iterable, Optional, Type, TypeVar

from .logging_utils import LOGGER_NAME
from .storage import get_storage

T = TypeVar("T")

_log = logging.getLogger(LOGGER_NAME)


def digest(payload: dict) -> str:
    """Store key of a calibration: a hash of everything it read."""
    text = json.dumps(payload, sort_keys=True, default=list)
    return hashlib.sha256(text.encode()).hexdigest()[:32]


def load(path: str, cls: Type[T], tuple_fields: Iterable[str]) -> Optional[T]:
    """The calibration stored at ``path``, or None on a miss or an entry that
    no longer matches ``cls``."""
    try:
        raw = get_storage().read_json(path)
    except Exception:  # noqa: BLE001 -- a miss, not an error
        return None
    if not raw:
        return None
    try:
        raw["snapshot"] = date.fromisoformat(raw["snapshot"])
        for name in tuple_fields:
            raw[name] = tuple(raw[name])
        return cls(**raw)
    except (KeyError, TypeError, ValueError):
        return None


def save(path: str, value, tuple_fields: Iterable[str], what: str) -> None:
    """Store a calibration; a failure is logged, the next restart recalibrates."""
    raw = dataclasses.asdict(value)
    raw["snapshot"] = value.snapshot.isoformat()
    for name in tuple_fields:
        raw[name] = list(getattr(value, name))
    try:
        get_storage().write_json(path, raw)
    except Exception:  # noqa: BLE001 -- the next restart recalibrates
        _log.exception("could not store the %s calibration", what)
