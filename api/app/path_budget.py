"""Price what fits in the time limit, instead of a 408 (issue #347).

A request for N paths that cannot finish within the route's timeout used to
end in a 408 with no value. Here it is priced with the M paths that fit, and
the response says so (a `time_limit_paths` warning): the value has no added
bias, only the larger standard error of M paths, which it reports.

No pilot run, so no pricing is slowed: the cost comes from the pricings
already done. For one request up to its path count and seed (the same
product, terms, model, market, device and greeks), a finished run of n paths
whose engine took t seconds bounds the cost of a path above by t / n -- it
folds in the fixed costs, so it over-states the cost and under-states M,
never the reverse. Only runs of at least MIN_OBSERVED_S count: on a shorter
one the fixed costs dominate and the bound says little. Without such a run
the request is priced as asked, as before. The engines are not touched.
"""

from __future__ import annotations

import json
import math
import re
import threading
from collections import OrderedDict
from typing import Optional

from pydantic import BaseModel

from .schemas import ModelWarning, PricingResponse

#: The least engine time of a run whose t / n is trusted as a bound.
MIN_OBSERVED_S = 2.0
#: The share of the route's timeout the engine may be planned for: the rest
#: is for the market reads, a calibration and the error of the bound.
ENGINE_SHARE = 0.75
#: The least path count it reduces to (the request models' own floor).
MIN_PATHS = 1_000
#: The multilevel engine's line in a run's diagnostics (mlqmc.hpp describe()).
_MULTILEVEL = "multilevel QMC ("
#: Requests remembered, least recently used dropped first.
MAX_KEYS = 512

_lock = threading.Lock()
#: key -> (the smallest engine seconds per path seen, each an upper bound;
#: the Sobol replicates of those runs, or None for pseudo-random paths).
_per_path: "OrderedDict[str, tuple[float, Optional[int]]]" = OrderedDict()
#: What a Sobol run simulated (simulation_engine.hpp, script_engine.cpp): the
#: engine rounds each replicate's points to the nearest power of two
#: (utils/sobol.hpp), up to ~1.41x the paths asked.
_SOBOL = re.compile(r"Sobol RQMC \((\d+) batches of (\d+) points")


def _ran(priced, asked: int) -> tuple[int, Optional[int]]:
    """(paths the engine simulated, its Sobol replicates or None)."""
    m = _SOBOL.search(priced.response.diagnostics or "")
    if m is None:
        return asked, None
    reps, points = int(m.group(1)), int(m.group(2))
    return reps * points, reps


def _key(product_id: str, req: BaseModel) -> Optional[str]:
    """The request up to its path count and seed; None when it has no path
    count to adjust (not Monte-Carlo, or priced to a precision)."""
    if "n_paths" not in type(req).model_fields:
        return None
    if getattr(req, "precision", None) is not None:
        return None
    terms = req.model_dump(mode="json", exclude={"n_paths", "seed", "precision"})
    return product_id + json.dumps(terms, sort_keys=True, default=str)


def paths_that_fit(
    product_id: str, req: BaseModel, timeout_s: float
) -> Optional[tuple[int, float]]:
    """(M, the seconds N paths would take) when the N paths asked would not
    finish in time; None to price as asked."""
    key = _key(product_id, req)
    if key is None:
        return None
    with _lock:
        seen = _per_path.get(key)
        if seen is not None:
            _per_path.move_to_end(key)
    if seen is None:
        return None
    per_path, reps = seen
    asked = int(req.n_paths)
    predicted = per_path * _rounded(asked, reps)
    budget = ENGINE_SHARE * timeout_s
    if predicted <= budget:
        return None
    fit = int(budget / per_path)
    if reps:
        # replicates x a power of two: the engine runs exactly that many
        fit = reps * 2 ** int(math.log2(max(fit // reps, 1)))
    fit = max(MIN_PATHS, fit)
    return (fit, predicted) if fit < asked else None


def _rounded(n: int, reps: Optional[int]) -> int:
    """The paths a Sobol run of n simulates (utils/sobol.hpp: each replicate
    at the power of two nearest n / reps, geometrically)."""
    if not reps:
        return n
    nd = n / reps
    if nd <= 1.0:
        return reps
    lower = 2 ** int(math.log2(nd))
    return reps * (2 * lower if nd * nd >= 2.0 * lower * lower else lower)


def record(product_id: str, req: BaseModel, priced) -> None:
    """Learn from a finished run (valuation.Priced): its engine seconds over
    its paths. A multilevel run is skipped: its levels, not n_paths, set its
    cost (the engine chooses it at run time, mlmc='auto')."""
    engine_s = priced.phases.get("engine")
    if engine_s is None or engine_s < MIN_OBSERVED_S:
        return
    if _MULTILEVEL in (priced.response.diagnostics or ""):
        return
    key = _key(product_id, req)
    if key is None:
        return
    paths, reps = _ran(priced, int(req.n_paths))
    bound = engine_s / paths
    with _lock:
        old = _per_path.get(key)
        _per_path[key] = (bound if old is None else min(old[0], bound), reps)
        _per_path.move_to_end(key)
        while len(_per_path) > MAX_KEYS:
            _per_path.popitem(last=False)


def warn(
    response: PricingResponse,
    asked: int,
    fit: int,
    predicted_s: float,
    timeout_s: float,
) -> PricingResponse:
    """The response of M paths, saying it is not the N asked."""
    message = (
        f"Not the paths asked: priced with {fit:,} of the {asked:,} paths, the "
        f"time limit. {asked:,} paths would take about {predicted_s:.0f} s, past "
        f"the server's {timeout_s:g} s. The standard error shown is that of "
        f"{fit:,} paths, about {math.sqrt(asked / fit):.1f}x what {asked:,} "
        "would give."
    )
    w = ModelWarning(code="time_limit_paths", severity="warning", message=message)
    return response.model_copy(
        update={
            "warnings": list(response.warnings) + [w],
            "diagnostics": response.diagnostics
            + f" | {fit} of the {asked} paths asked: the time limit",
        }
    )


def forget() -> None:
    """Tests: drop what was learnt."""
    with _lock:
        _per_path.clear()
