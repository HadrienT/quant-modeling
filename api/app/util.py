"""Small helpers several API modules share."""

from __future__ import annotations

import math
from datetime import date, datetime, timezone


def today_utc() -> date:
    """Today's date in UTC — matches the C++ Date::today()."""
    return datetime.now(timezone.utc).date()


def norm_cdf(x: float) -> float:
    """Standard normal distribution function, through erfc so that the lower
    tail keeps its relative precision."""
    return 0.5 * math.erfc(-x / math.sqrt(2.0))
