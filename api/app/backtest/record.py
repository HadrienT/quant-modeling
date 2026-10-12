"""The `backtest.run` audit event and its replay
(blueprint/wp/24-backtest.md, lot B8, slice 5).

Same request, same data (equal fingerprints) and same build give the same
figures to the bit: the engine is deterministic at a fixed seed and its result
does not depend on the number of threads. The verdicts are those of a
valuation's replay (`replay.py`): `drifted_inputs` names the tables whose
fingerprint moved, `drifted_code` the builds.
"""

from __future__ import annotations

import os
from typing import Any, Callable

from pydantic import BaseModel, ValidationError

from .. import replay as valuation_replay
from ..audit.envelope import lib_build_sha
from ..audit.payloads import (
    BacktestEngine,
    BacktestRunPayload,
    CodeVersion,
    ValuationTiming,
)
from ..request_context import current_ip_hash
from ..valuation import value_hash
from . import engine, market  # noqa: F401 — market: re-exported for the router
from . import schemas as s

EVENT_TYPE = "backtest.run"


def _strategy_headline(r: s.StrategyResponse) -> dict[str, float | None]:
    metrics = r.strategy.report.metrics

    def value(name: str) -> float | None:
        return metrics[name].value if name in metrics else None

    return {
        "final_value": r.strategy.value[-1],
        "sharpe_ratio": value("sharpe_ratio"),
        "sharpe_ratio_lower": (
            metrics["sharpe_ratio"].lower if "sharpe_ratio" in metrics else None
        ),
        "annualized_return": value("annualized_return"),
        "max_drawdown": value("max_drawdown"),
        "factor_alpha": r.factor_regression.alpha.value,
        "sharpe_difference": r.sharpe_difference.analytical.value,
        "bootstrap_p_value": r.sharpe_difference.bootstrap_p_value,
    }


def _validation_headline(r: s.ValidationResponse) -> dict[str, float | None]:
    return {
        "trials": float(r.deflated_sharpe.trials),
        "best_sharpe": r.deflated_sharpe.sharpe,
        "deflated_sharpe_ratio": r.deflated_sharpe.deflated_sharpe_ratio,
        "overfitting_probability": r.overfitting.probability,
        "walk_forward_folds": float(len(r.walk_forward.folds)),
    }


#: kind → (request model, run, headline)
KINDS: dict[str, tuple[type[BaseModel], Callable, Callable]] = {
    "strategy": (s.StrategyRequest, engine.run_strategy, _strategy_headline),
    "validation": (s.ValidationRequest, engine.run_validation, _validation_headline),
}


def _code() -> CodeVersion:
    return CodeVersion(
        api_sha=os.getenv("COMMIT_SHA", "dev"), lib_build=lib_build_sha()
    )


def payload(kind: str, req: BaseModel, response: Any) -> BacktestRunPayload:
    request = req.model_dump(mode="json")
    return BacktestRunPayload(
        kind=kind,
        request=request,
        request_hash=value_hash(request),
        fingerprints=response.data.fingerprints,
        engine=BacktestEngine(**response.engine.model_dump()),
        headline=KINDS[kind][2](response),
        timing=ValuationTiming(duration_ms=response.compute_ms),
        code=_code(),
        ip_hash=current_ip_hash(),
    )


# ── Replay ───────────────────────────────────────────────────────────────────


class DriftedFigure(BaseModel):
    name: str
    recorded: float | None
    replayed: float | None


class DriftedTable(BaseModel):
    name: str
    recorded_fingerprint: str | None
    current_fingerprint: str | None


class BacktestReplayResponse(BaseModel):
    event_id: str
    kind: str
    outcome: valuation_replay.ReplayOutcome
    recorded_headline: dict[str, float | None]
    replayed_headline: dict[str, float | None]
    #: The figures that differ, whatever the outcome.
    differences: list[DriftedFigure]
    drifted_inputs: list[DriftedTable]
    recorded_engine: BacktestEngine
    current_engine: BacktestEngine
    recorded_code: valuation_replay.CodeBuild
    current_code: valuation_replay.CodeBuild


def _same(a: float | None, b: float | None) -> bool:
    if a is None or b is None:
        return a is None and b is None
    return valuation_replay._same(a, b)


def replay(event: dict[str, Any]) -> BacktestReplayResponse:
    body = event["payload"]
    kind = body.get("kind")
    if kind not in KINDS:
        raise valuation_replay.NotAValuation(f"unknown backtest kind {kind!r}")
    model, run, headline = KINDS[kind]
    try:
        req = model.model_validate(body["request"])
    except ValidationError as exc:
        raise valuation_replay.NotAValuation(
            f"the recorded request is no longer valid for a {kind} backtest: {exc}"
        ) from exc

    response = run(req)

    recorded = dict(body.get("headline", {}))
    replayed = headline(response)
    differences = [
        DriftedFigure(name=n, recorded=recorded.get(n), replayed=replayed.get(n))
        for n in sorted(recorded.keys() | replayed.keys())
        if not _same(recorded.get(n), replayed.get(n))
    ]
    before = dict(body.get("fingerprints", {}))
    after = response.data.fingerprints
    drifted = [
        DriftedTable(
            name=n,
            recorded_fingerprint=before.get(n),
            current_fingerprint=after.get(n),
        )
        for n in sorted(before.keys() | after.keys())
        if before.get(n) != after.get(n)
    ]
    recorded_engine = BacktestEngine(**body["engine"])
    current_engine = BacktestEngine(**response.engine.model_dump())
    code = body.get("code", {})
    recorded_code = valuation_replay.CodeBuild(
        api_sha=str(code.get("api_sha", "unknown")),
        lib_build=str(code.get("lib_build", "unknown")),
    )
    current = _code()
    current_code = valuation_replay.CodeBuild(
        api_sha=current.api_sha, lib_build=current.lib_build
    )

    # The engine's build decides the figures; the API's only reshapes them.
    if drifted:
        outcome = valuation_replay.ReplayOutcome.DRIFTED_INPUTS
    elif recorded_engine != current_engine:
        outcome = valuation_replay.ReplayOutcome.DRIFTED_CODE
    elif not differences:
        outcome = valuation_replay.ReplayOutcome.REPRODUCED
    elif recorded_code != current_code:
        outcome = valuation_replay.ReplayOutcome.DRIFTED_CODE
    else:
        outcome = valuation_replay.ReplayOutcome.NOT_REPRODUCED

    return BacktestReplayResponse(
        event_id=str(event["event_id"]),
        kind=kind,
        outcome=outcome,
        recorded_headline=recorded,
        replayed_headline=replayed,
        differences=differences,
        drifted_inputs=drifted,
        recorded_engine=recorded_engine,
        current_engine=current_engine,
        recorded_code=recorded_code,
        current_code=current_code,
    )
