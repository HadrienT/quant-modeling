"""The Black-Scholes example of Huge & Savine (2020), section 3.1.

The network must learn the price at T1 of a call expiring at T2, as a
function of the spot at T1, from one simulated payoff per path. The pathwise
differential of the payoff with respect to the spot at T1 is known in closed
form here, which makes this the reference case: the truth is the
Black-Scholes formula.
"""

from __future__ import annotations

import math

import torch

from .twin import DTYPE

STRIKE = 1.10
VOL = 0.20
T1 = 1.0
T2 = 2.0


def simulate(
    paths: int,
    seed: int,
    spot: float = 1.0,
    vol_multiplier: float = 1.5,
    antithetic: bool = False,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Spot at T1, payoff at T2, and d(payoff)/d(spot at T1), each (paths, 1).

    The spot at T1 is drawn with an inflated volatility so that the training
    set covers more than the likely region, as in the paper. With
    `antithetic`, each label is the mean over the two mirrored paths from T1
    to T2: far less noise for the same number of training rows.
    """
    generator = torch.Generator().manual_seed(seed)
    g1 = torch.randn(paths, 1, generator=generator, dtype=DTYPE)
    g2 = torch.randn(paths, 1, generator=generator, dtype=DTYPE)
    vol0 = VOL * vol_multiplier
    s1 = spot * torch.exp(-0.5 * vol0 * vol0 * T1 + vol0 * math.sqrt(T1) * g1)
    tau = T2 - T1
    signs = (1.0, -1.0) if antithetic else (1.0,)
    payoff = torch.zeros_like(s1)
    differential = torch.zeros_like(s1)
    for sign in signs:
        s2 = s1 * torch.exp(-0.5 * VOL * VOL * tau + VOL * math.sqrt(tau) * sign * g2)
        payoff += torch.clamp(s2 - STRIKE, min=0.0) / len(signs)
        differential += torch.where(s2 > STRIKE, s2 / s1, torch.zeros_like(s2)) / len(
            signs
        )
    return s1, payoff, differential


def price_and_delta(spot: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    """Black-Scholes price and delta at T1 of the call expiring at T2 (zero rates)."""
    tau = T2 - T1
    d1 = (torch.log(spot / STRIKE) + 0.5 * VOL * VOL * tau) / (VOL * math.sqrt(tau))
    d2 = d1 - VOL * math.sqrt(tau)
    delta = torch.special.ndtr(d1)
    return spot * delta - STRIKE * torch.special.ndtr(d2), delta
