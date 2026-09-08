"""Pluggable key/value + prefix storage — blueprint WP 04.

Default backend is local JSON files on a mounted volume, so the app runs with
NO Google Cloud credentials. GCS stays available (QM_STORAGE=gcs) but is no
longer required.
"""

from __future__ import annotations

import json
import os
import threading
from pathlib import Path
from typing import Dict, List, Protocol


class Storage(Protocol):
    def read_json(self, key: str) -> dict | None: ...
    def write_json(self, key: str, value: dict) -> None: ...
    def delete(self, key: str) -> bool: ...
    def list_keys(self, prefix: str) -> List[str]: ...


class LocalJsonStorage:
    """One file per key under a root directory. Thread-safe for our scale."""

    def __init__(self, root: str | os.PathLike) -> None:
        self._root = Path(root)
        self._root.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()

    def _path(self, key: str) -> Path:
        safe = key.replace("..", "_").lstrip("/")
        p = (self._root / safe).with_suffix(".json") if not safe.endswith(".json") else self._root / safe
        p.parent.mkdir(parents=True, exist_ok=True)
        return p

    def read_json(self, key: str) -> dict | None:
        p = self._path(key)
        if not p.exists():
            return None
        with self._lock:
            return json.loads(p.read_text())

    def write_json(self, key: str, value: dict) -> None:
        p = self._path(key)
        with self._lock:
            tmp = p.with_suffix(".tmp")
            tmp.write_text(json.dumps(value, indent=2))
            tmp.replace(p)

    def delete(self, key: str) -> bool:
        p = self._path(key)
        if not p.exists():
            return False
        with self._lock:
            p.unlink()
        return True

    def list_keys(self, prefix: str) -> List[str]:
        base = self._path(prefix).parent if not prefix.endswith("/") else self._root / prefix.strip("/")
        if not base.exists():
            return []
        out: List[str] = []
        for f in base.rglob("*.json"):
            rel = f.relative_to(self._root).with_suffix("")
            out.append(str(rel))
        return out


class GcsStorage:
    def __init__(self, bucket: str) -> None:
        from google.cloud import storage as gcs  # imported only when selected

        creds = os.getenv("GOOGLE_APPLICATION_CREDENTIALS")
        client = gcs.Client.from_service_account_json(creds) if creds else gcs.Client()
        self._bucket = client.bucket(bucket)

    def _name(self, key: str) -> str:
        return key if key.endswith(".json") else f"{key}.json"

    def read_json(self, key: str) -> dict | None:
        blob = self._bucket.blob(self._name(key))
        if not blob.exists():
            return None
        return json.loads(blob.download_as_text())

    def write_json(self, key: str, value: dict) -> None:
        self._bucket.blob(self._name(key)).upload_from_string(
            json.dumps(value, indent=2), content_type="application/json"
        )

    def delete(self, key: str) -> bool:
        blob = self._bucket.blob(self._name(key))
        if not blob.exists():
            return False
        blob.delete()
        return True

    def list_keys(self, prefix: str) -> List[str]:
        return [
            b.name[:-5] if b.name.endswith(".json") else b.name
            for b in self._bucket.list_blobs(prefix=prefix)
            if b.name.endswith(".json")
        ]


_INSTANCE: Storage | None = None
_CACHE: Dict[str, Storage] = {}


def get_storage() -> Storage:
    global _INSTANCE
    if _INSTANCE is not None:
        return _INSTANCE
    backend = os.getenv("QM_STORAGE", "local").lower()
    if backend == "gcs":
        _INSTANCE = GcsStorage(os.getenv("GCS_PORTFOLIO_BUCKET", "quant-portfolios"))
    else:
        _INSTANCE = LocalJsonStorage(os.getenv("QM_DATA_DIR", "./data"))
    return _INSTANCE
