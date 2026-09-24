"""Replay of a recorded valuation — blueprint WP 18e.

Re-runs the pricing a `pricing.valuation` event describes, and says what came
of it. Never a silent "it passes": the verdict names its cause.

| Outcome           | Meaning                                                        |
|-------------------|----------------------------------------------------------------|
| `reproduced`      | same market inputs (equal hashes), same code, same price       |
| `drifted_inputs`  | a market datum was revised since (data-ingest upserts): which  |
| `drifted_code`    | the API or the native library changed: from which build to which, and the price gap |
| `not_reproduced`  | same inputs, same code, different price                        |

The fourth outcome is a departure from the WP's three: with identical inputs
and code, a different price means the pricing is not deterministic at a fixed
seed — "a bug to fix, not a tolerance to widen" (WP 18e). Folding it into one
of the other three would hide exactly that bug.

The tolerance is relative 1e-12, i.e. equality up to the last bits of a
double: a replay runs the same code on the same inputs with the same seed, so
anything larger is a real difference.
"""

from __future__ import annotations

import math
import os
from enum import Enum
from typing import Any

from pydantic import BaseModel, ValidationError

from . import valuation
from .audit.envelope import lib_build_sha

REPRODUCTION_RTOL = 1e-12


class ReplayOutcome(str, Enum):
    REPRODUCED = "reproduced"
    DRIFTED_INPUTS = "drifted_inputs"
    DRIFTED_CODE = "drifted_code"
    NOT_REPRODUCED = "not_reproduced"


class CodeBuild(BaseModel):
    api_sha: str
    lib_build: str


class DriftedInput(BaseModel):
    name: str
    recorded_hash: str | None
    current_hash: str | None
    recorded_as_of: str | None
    current_as_of: str | None


class ReplayResponse(BaseModel):
    event_id: str
    product: str
    outcome: ReplayOutcome
    recorded_npv: float
    replayed_npv: float
    npv_difference: float
    drifted_inputs: list[DriftedInput]
    recorded_code: CodeBuild
    current_code: CodeBuild


class NotAValuation(ValueError):
    """The event exists but cannot be replayed (wrong type, unknown product,
    a request the current API no longer accepts)."""


def _same(a: float, b: float) -> bool:
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    return abs(a - b) <= REPRODUCTION_RTOL * max(1.0, abs(a), abs(b))


def _drifted(
    recorded: list[dict[str, Any]], current: list[valuation.MarketInput]
) -> list[DriftedInput]:
    before = {i["name"]: i for i in recorded}
    after = {i.name: i for i in current}
    drifted = []
    for name in sorted(before.keys() | after.keys()):
        b, a = before.get(name), after.get(name)
        b_hash = b["value_hash"] if b else None
        a_hash = a.value_hash if a else None
        if b_hash != a_hash:
            drifted.append(
                DriftedInput(
                    name=name,
                    recorded_hash=b_hash,
                    current_hash=a_hash,
                    recorded_as_of=b.get("as_of") if b else None,
                    current_as_of=a.as_of if a else None,
                )
            )
    return drifted


def replay(event: dict[str, Any]) -> ReplayResponse:
    if event.get("type") != "pricing.valuation":
        raise NotAValuation(
            f"event {event.get('event_id')} is a {event.get('type')!r}, "
            "not a pricing.valuation"
        )
    payload = event["payload"]
    product_id = payload.get("product")
    product = valuation.PRODUCTS.get(product_id)
    if product is None:
        raise NotAValuation(f"unknown product {product_id!r}")
    try:
        req = product.request.model_validate(payload["request"])
    except ValidationError as exc:
        raise NotAValuation(
            f"the recorded request is no longer valid for {product_id}: {exc}"
        ) from exc

    priced = valuation.price(product_id, req)

    recorded_npv = float(payload["result"]["npv"])
    replayed_npv = float(priced.response.npv)
    code = payload.get("code", {})
    recorded_code = CodeBuild(
        api_sha=str(code.get("api_sha", "unknown")),
        lib_build=str(code.get("lib_build", "unknown")),
    )
    current_code = CodeBuild(
        api_sha=os.getenv("COMMIT_SHA", "dev"), lib_build=lib_build_sha()
    )
    drifted = _drifted(payload.get("market_inputs", []), priced.market_inputs)

    if drifted:
        outcome = ReplayOutcome.DRIFTED_INPUTS
    elif recorded_code != current_code:
        outcome = ReplayOutcome.DRIFTED_CODE
    elif _same(recorded_npv, replayed_npv):
        outcome = ReplayOutcome.REPRODUCED
    else:
        outcome = ReplayOutcome.NOT_REPRODUCED

    return ReplayResponse(
        event_id=str(event["event_id"]),
        product=product_id,
        outcome=outcome,
        recorded_npv=recorded_npv,
        replayed_npv=replayed_npv,
        npv_difference=replayed_npv - recorded_npv,
        drifted_inputs=drifted,
        recorded_code=recorded_code,
        current_code=current_code,
    )
