"""`record_fallback()` — the single entry point for every live-data or
default-value fallback (WP §5.1). `kind` is the closed `FallbackKind`
enumeration: adding a member means touching this file and `payloads.py`, which
is the point — a fallback cannot quietly stop being tracked.
"""

from __future__ import annotations

from .emit import emit
from .payloads import FallbackKind, FallbackPayload

__all__ = ["record_fallback"]


def record_fallback(
    kind: FallbackKind, *, detail: str = "", **context: str | int | float | None
) -> None:
    emit(
        "data.fallback",
        FallbackPayload(kind=kind, detail=detail, context=dict(context)),
    )
