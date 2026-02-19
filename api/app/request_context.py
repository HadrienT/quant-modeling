"""Request context utilities for tracking request-level state."""

from contextvars import ContextVar

# Context variable to track cache hits in the current request
cache_hit_context: ContextVar[bool] = ContextVar("cache_hit", default=False)


def set_cache_hit():
    """Mark current request as cache hit."""
    cache_hit_context.set(True)


def is_cache_hit() -> bool:
    """Check if current request hit cache."""
    return cache_hit_context.get()


def reset_cache_hit():
    """Reset cache hit status (called at start of each request)."""
    cache_hit_context.set(False)
