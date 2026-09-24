"""What a mark is made of: its value, the market inputs it read, and the
model that turned them into a price (portfolio_valuation.py computes marks,
portfolio_models.py chooses and runs the model)."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date
from typing import Dict, List, Optional


@dataclass
class Input:
    name: str
    status: str  # observed | stale | proxied | default
    value: Optional[float]
    as_of: Optional[date] = None


@dataclass
class ModelParam:
    """One parameter of the model a mark was priced with. `status` says
    where it comes from: observed (market data of that day), calibrated
    (fitted to that day's option prices), proxied (an estimate standing in
    for a missing quote), contract (a term of the product) or default (the
    value typed with the trade, for want of anything better)."""

    name: str
    value: Optional[float] = None
    text: Optional[str] = None  # when the value is not a number
    status: str = "observed"
    source: str = ""


@dataclass
class ModelInfo:
    """The model behind a mark, as the hover on a position shows it."""

    model: str  # e.g. "Dupire local volatility"
    engine: str  # e.g. "Monte-Carlo (Savine scripting engine)"
    why: str  # why this model for this product
    params: List[ModelParam] = field(default_factory=list)
    std_error: Optional[float] = None  # Monte-Carlo standard error of the mark


@dataclass
class Mark:
    value: Optional[float]  # per unit, instrument currency; None when unavailable
    currency: str
    inputs: List[Input] = field(default_factory=list)
    greeks: Dict[str, Optional[float]] = field(default_factory=dict)
    note: Optional[str] = None  # why there is no value, or what to do
    model: Optional[ModelInfo] = None


__all__ = ["Input", "Mark", "ModelInfo", "ModelParam"]
