import time
from collections import OrderedDict
from threading import Lock
from typing import Generic, Optional, Tuple, TypeVar

K = TypeVar("K")
V = TypeVar("V")


class TTLCache(Generic[K, V]):
    def __init__(self, max_size: int = 256, ttl_seconds: int = 60) -> None:
        self._max_size = max_size
        self._ttl_seconds = ttl_seconds
        self._store: "OrderedDict[K, Tuple[float, V]]" = OrderedDict()
        self._lock = Lock()

    def get(self, key: K) -> Optional[V]:
        now = time.time()
        with self._lock:
            if key not in self._store:
                return None
            expires_at, value = self._store[key]
            if expires_at <= now:
                self._store.pop(key, None)
                return None
            self._store.move_to_end(key)
            return value

    def set(self, key: K, value: V) -> None:
        expires_at = time.time() + self._ttl_seconds
        with self._lock:
            self._store[key] = (expires_at, value)
            self._store.move_to_end(key)
            while len(self._store) > self._max_size:
                self._store.popitem(last=False)