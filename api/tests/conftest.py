import sys
from pathlib import Path

# `app` lives in api/, which is not on the path when pytest runs from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import pytest  # noqa: E402


@pytest.fixture(autouse=True)
def _isolated_store_and_caches(tmp_path, monkeypatch):
    """Each test starts with an empty store (storage.py) and empty
    calibration caches: a calibration stored by one test is never served to
    the next."""
    from app import market_snapshot, stochastic_vol, storage

    monkeypatch.setattr(
        storage, "_INSTANCE", storage.LocalJsonStorage(tmp_path / "store")
    )
    market_snapshot._surfaces.clear()
    stochastic_vol._cache.clear()
    yield
    market_snapshot._surfaces.clear()
    stochastic_vol._cache.clear()
