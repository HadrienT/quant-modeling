import sys
from pathlib import Path

# `app` lives in api/, which is not on the path when pytest runs from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import pytest  # noqa: E402


@pytest.fixture(autouse=True)
def _isolated_store_and_caches(tmp_path, monkeypatch):
    """Each test starts with an empty store (storage.py) and empty
    calibration caches: a calibration stored by one test is never served to
    the next, nor written to the repository's ./data. Tests import the API
    both as `app` and as `api.app`, two module trees: both are isolated."""
    import importlib

    trees = []
    for root in ("app", "api.app"):
        try:
            trees.append(
                [
                    importlib.import_module(f"{root}.{m}")
                    for m in ("storage", "market_snapshot", "stochastic_vol")
                ]
            )
        except ImportError:
            pass
    for storage, market_snapshot, stochastic_vol in trees:
        monkeypatch.setattr(
            storage, "_INSTANCE", storage.LocalJsonStorage(tmp_path / "store")
        )
        market_snapshot._surfaces.clear()
        stochastic_vol._cache.clear()
    yield
    for _, market_snapshot, stochastic_vol in trees:
        market_snapshot._surfaces.clear()
        stochastic_vol._cache.clear()
