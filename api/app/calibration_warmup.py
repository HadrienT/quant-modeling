"""Stochastic-vol calibrations made before anyone asks (issue #90).

The first Heston or SLV pricing of a ticker on a new snapshot calibrates the
surface, then Heston and the leverage: seconds a request would otherwise wait
for. A background thread does it instead, at API start and then every
INTERVAL_SECONDS, for every ticker data-ingest tracks and every rate it was
last calibrated at (stochastic_vol.remembered_rates) -- the rate is an input
of the calibration, and the requests choose it. A ticker nobody has priced
under a stochastic model yet has no rate to warm, and is left alone.

Nothing here is required: stochastic_vol.calibrate stores what it computes,
so a warm-up that fails only means the next request calibrates. Enabled by
QM_CALIBRATION_WARMUP=1 (the compose files set it).
"""

from __future__ import annotations

import logging
import os
import threading
from datetime import date, timedelta
from typing import List, Optional, Tuple

from . import db, market_snapshot, stochastic_vol
from .logging_utils import LOGGER_NAME

#: How often the store is checked for new snapshots (data-ingest adds one a
#: day; a check with nothing new costs a few queries).
INTERVAL_SECONDS = 30 * 60
#: A ticker counts as tracked if it has a snapshot this recent.
TRACKED_WITHIN_DAYS = 7

_log = logging.getLogger(LOGGER_NAME)
_stop = threading.Event()
_thread: Optional[threading.Thread] = None


def warm_once(today: date) -> List[Tuple[str, float, str]]:
    """Calibrate (or find stored) every tracked ticker's latest surface at
    each remembered rate. Returns (ticker, rate, outcome) per pair."""
    done: List[Tuple[str, float, str]] = []
    for ticker in db.options_chain_tickers(today - timedelta(days=TRACKED_WITHIN_DAYS)):
        for rate in stochastic_vol.remembered_rates(ticker):
            if _stop.is_set():
                return done
            try:
                market = market_snapshot.local_vol_market(ticker, rate, today)
                sv = stochastic_vol.calibrate(market)
                done.append((ticker, rate, f"ready for {sv.snapshot.isoformat()}"))
            except (
                market_snapshot.MarketDataUnavailable,
                stochastic_vol.CalibrationUnavailable,
            ) as exc:
                done.append((ticker, rate, f"skipped: {exc}"))
    return done


def _run() -> None:
    while not _stop.is_set():
        try:
            for ticker, rate, outcome in warm_once(date.today()):
                _log.info("calibration warm-up %s at %.4g: %s", ticker, rate, outcome)
        except Exception:  # noqa: BLE001 -- the database may be down; retry later
            _log.exception("calibration warm-up failed")
        _stop.wait(INTERVAL_SECONDS)


def enabled() -> bool:
    return os.getenv("QM_CALIBRATION_WARMUP", "0").lower() in ("1", "true", "yes")


def start() -> None:
    global _thread
    if _thread is not None or not enabled():
        return
    _stop.clear()
    _thread = threading.Thread(target=_run, name="calibration-warmup", daemon=True)
    _thread.start()


def stop() -> None:
    global _thread
    _stop.set()
    _thread = None
