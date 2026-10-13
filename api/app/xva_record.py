"""The audit record of an xVA run, and its replay (blueprint/wp/23-xva.md
§14.5, issue #141).

Every run of `/api/xva/netting-set` and `/api/xva/portfolio` emits an
`xva.valuation` event: the request, the seed, the calibrated model and the
hashes of what was read from the store. `replay` runs the request again and
says what came of it, with the verdicts of the pricing replay (`replay.py`).

Two limits, stated in the response or as an explicit refusal:

- The market is the one the store holds **now**: the xVA service reads the
  latest curve, vols and spreads, not those of a past date. A replay made
  after data-ingest has run again reports `drifted_inputs` and names them; it
  is `reproduced` only while the market inputs are unchanged.
- A netting set of a portfolio is recorded with the terms of its trades, not
  with the ledger they came from: it cannot be run again, and is refused.
"""

from __future__ import annotations

import os
from typing import Any

from pydantic import BaseModel, ValidationError

from . import replay as valuation_replay
from . import valuation, xva
from .audit.envelope import lib_build_sha
from .audit.payloads import (
    CodeVersion,
    EngineSpec,
    MarketInput,
    MarketInputStatus,
    ModelSpec,
    ValuationTiming,
    XvaResult,
    XvaValuationPayload,
)
from .request_context import current_ip_hash
from .xva_schemas import XvaRequest, XvaResponse

EVENT_TYPE = "xva.valuation"


def _market_inputs(response: XvaResponse) -> list[MarketInput]:
    return [
        MarketInput(
            name=name,
            source=source,
            as_of=as_of,
            status=MarketInputStatus.OBSERVED,
            value_hash=valuation.value_hash(value),
        )
        for name, source, as_of, value in xva.market_inputs(response)
    ]


def _result(response: XvaResponse) -> XvaResult:
    return XvaResult(
        cva=response.adjustments.cva.value,
        cva_std_error=response.adjustments.cva.error,
        dva=response.adjustments.dva.value,
        dva_std_error=response.adjustments.dva.error,
        epe=response.exposure.epe,
        peak_pfe=max(response.exposure.pfe),
    )


def _code() -> CodeVersion:
    return CodeVersion(
        api_sha=os.getenv("COMMIT_SHA", "dev"), lib_build=lib_build_sha()
    )


def payload(
    req: XvaRequest, response: XvaResponse, trades: bool = False
) -> XvaValuationPayload:
    request = req.model_dump(mode="json")
    if trades:
        # A netting set of a portfolio is not named by its id: its trades
        # are part of what was asked. The portfolio itself is not recorded.
        request["trades"] = [
            t.model_dump(
                mode="json",
                include={
                    "kind", "currency", "quantity", "payer", "notional",
                    "fixed_rate", "foreign_rate", "start", "tenor", "maturity",
                },
            )
            for t in response.trades
        ]  # fmt: skip
    hw = response.market.hull_white
    return XvaValuationPayload(
        request=request,
        request_hash=valuation.value_hash(request),
        model=ModelSpec(
            name="hull_white_one_factor",
            params={"mean_reversion": hw.mean_reversion, "sigma": hw.sigma},
        ),
        engine=EngineSpec(
            name="hull_white_exposure",
            n_paths=response.paths,
            seed=response.seed,
            scheme="exact",
            device=response.device,
        ),
        market_inputs=_market_inputs(response),
        result=_result(response),
        timing=ValuationTiming(duration_ms=response.compute_ms),
        code=_code(),
        ip_hash=current_ip_hash(),
    )


# ── Replay ───────────────────────────────────────────────────────────────────


class DriftedFigure(BaseModel):
    name: str
    recorded: float
    replayed: float


class XvaReplayResponse(BaseModel):
    event_id: str
    portfolio: str
    outcome: valuation_replay.ReplayOutcome
    recorded: XvaResult
    replayed: XvaResult
    #: The figures that differ, whatever the outcome.
    differences: list[DriftedFigure]
    drifted_inputs: list[valuation_replay.DriftedInput]
    #: Where the paths ran: the GPU and the CPU agree to 1e-12, not bit for bit.
    recorded_device: str | None
    replayed_device: str
    recorded_code: valuation_replay.CodeBuild
    current_code: valuation_replay.CodeBuild


def replay(event: dict[str, Any]) -> XvaReplayResponse:
    body = event["payload"]
    request = dict(body.get("request", {}))
    if request.get("portfolio") == "ledger":
        raise valuation_replay.NotAValuation(
            "this xVA is that of a netting set of a portfolio: the event records "
            "the terms of its trades, not the ledger they came from, so it cannot "
            "be run again"
        )
    try:
        req = XvaRequest.model_validate(request)
    except ValidationError as exc:
        raise valuation_replay.NotAValuation(
            f"the recorded request is no longer valid for an xVA: {exc}"
        ) from exc

    response = xva.compute(req)

    recorded = XvaResult(**body["result"])
    replayed = _result(response)
    before, after = recorded.model_dump(), replayed.model_dump()
    differences = [
        DriftedFigure(name=n, recorded=before[n], replayed=after[n])
        for n in before
        if not valuation_replay._same(before[n], after[n])
    ]
    drifted = valuation_replay._drifted(
        body.get("market_inputs", []), _market_inputs(response)
    )
    code = body.get("code", {})
    recorded_code = valuation_replay.CodeBuild(
        api_sha=str(code.get("api_sha", "unknown")),
        lib_build=str(code.get("lib_build", "unknown")),
    )
    current = _code()
    current_code = valuation_replay.CodeBuild(
        api_sha=current.api_sha, lib_build=current.lib_build
    )

    if drifted:
        outcome = valuation_replay.ReplayOutcome.DRIFTED_INPUTS
    elif recorded_code != current_code:
        outcome = valuation_replay.ReplayOutcome.DRIFTED_CODE
    elif not differences:
        outcome = valuation_replay.ReplayOutcome.REPRODUCED
    else:
        outcome = valuation_replay.ReplayOutcome.NOT_REPRODUCED

    return XvaReplayResponse(
        event_id=str(event["event_id"]),
        portfolio=req.portfolio,
        outcome=outcome,
        recorded=recorded,
        replayed=replayed,
        differences=differences,
        drifted_inputs=drifted,
        recorded_device=body.get("engine", {}).get("device"),
        replayed_device=response.device,
        recorded_code=recorded_code,
        current_code=current_code,
    )
