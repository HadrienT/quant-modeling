"""A small in-process cache for costly, deterministic computations keyed by
their inputs: least recently used entries go first, and two threads asking
for the same missing key compute it once (the second waits for the first)."""

from __future__ import annotations

import threading
from collections import OrderedDict
from typing import Callable, Dict, Generic, Hashable, TypeVar

V = TypeVar("V")


class KeyedCache(Generic[V]):
    def __init__(self, size: int) -> None:
        self._size = size
        self._values: "OrderedDict[Hashable, V]" = OrderedDict()
        self._locks: Dict[Hashable, threading.Lock] = {}
        self._guard = threading.Lock()

    def get(self, key: Hashable, compute: Callable[[], V]) -> V:
        with self._guard:
            if key in self._values:
                self._values.move_to_end(key)
                return self._values[key]
            lock = self._locks.setdefault(key, threading.Lock())
        with lock:
            with self._guard:
                if key in self._values:
                    return self._values[key]
            value = compute()
            with self._guard:
                self._values[key] = value
                while len(self._values) > self._size:
                    self._values.popitem(last=False)
                self._locks.pop(key, None)
            return value

    def clear(self) -> None:
        with self._guard:
            self._values.clear()
