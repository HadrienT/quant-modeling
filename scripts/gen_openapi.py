#!/usr/bin/env python3
"""Dump the FastAPI OpenAPI schema to a file.

The pybind11 ``quantmodeling`` module is replaced by a recursive stub: the
OpenAPI *shape* is defined entirely by the Pydantic models and route
signatures, none of which need the native extension. This keeps the
``api-contract`` CI job fast and independent of the C++ build.

Usage:
    python scripts/gen_openapi.py web/openapi.json
"""

from __future__ import annotations

import json
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class _Any:
    """Answers any attribute access / call / index so module import succeeds."""

    def __getattr__(self, _name: str) -> "_Any":
        return _Any()

    def __call__(self, *_a: object, **_k: object) -> "_Any":
        return _Any()

    def __getitem__(self, _k: object) -> "_Any":
        return _Any()

    def __iter__(self):
        return iter(())


def _install_stub() -> None:
    mod = types.ModuleType("quantmodeling")
    mod.__getattr__ = lambda _name: _Any()  # type: ignore[attr-defined]
    sys.modules["quantmodeling"] = mod
    # auth.py fails fast without a real secret; provide one for the schema dump.
    import os

    os.environ.setdefault("JWT_SECRET", "openapi-dump-secret-not-for-runtime")
    os.environ.setdefault("QM_STORAGE", "local")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    _install_stub()
    sys.path.insert(0, str(ROOT))

    from api.app.main import app  # noqa: E402  (after stub + sys.path)

    schema = app.openapi()
    out = Path(sys.argv[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(schema, indent=2, sort_keys=True) + "\n")
    print(f"wrote {out} — {len(schema.get('paths', {}))} paths")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
