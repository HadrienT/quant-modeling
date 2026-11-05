"""Monte-Carlo convergence study: the engine's standard error measured at
several path counts, not drawn from the 1/sqrt(N) law it is meant to test.

The product is priced again, by the same engine with the same estimator
(sampler, antithetics, control variates, RQMC batches: whatever the request
asks for), at N/2, N/4, ..., N/32 paths, each run with its own seed so that
the points are independent estimates.

The point at N paths is not recomputed: it is the request's own pricing,
which the caller already has (the pricing route returns it for the same
request, bit for bit). Recomputing it would double the cost of the study,
N/2 + ... + N/32 being just under N. The caller adds it as the last point
and fits the slope of ln(std error) on ln(paths) over all of them
(web/src/features/pricing/convergenceSlope.ts): -1/2 for plain Monte Carlo;
randomized QMC or a good control variate can do better, and a slope far
from -1/2 on a pseudo-random run is a sign the error estimate is not to be
trusted (a payoff driven by rare paths, too few of them).
"""

from __future__ import annotations

from typing import List

from pydantic import BaseModel

from . import valuation
from .schemas import ConvergencePoint, ConvergenceResponse

#: Halvings below the requested path count: points at N/2, ..., N/32.
LEVELS = 5
#: No point below this many paths: the standard error of the standard error
#: is about 1/sqrt(2N), already 2 % here.
MIN_PATHS = 1_000

#: The pricing products whose engine is a Monte Carlo when asked to be:
#: those with a path count. A request with an `engine` field must ask for
#: "mc"; the others are Monte Carlo whatever their inputs.
MC_PRODUCTS = tuple(
    k for k, p in valuation.PRODUCTS.items() if "n_paths" in p.request.model_fields
)


class NotMonteCarlo(ValueError):
    """The request does not price by Monte Carlo: nothing to converge."""


def ladder(req: BaseModel) -> List[BaseModel]:
    """The requests of the study, smallest path count first: N/32 ... N/2,
    not N itself (the caller's own pricing). Each halving keeps every other
    input and moves the seed; a path count the request model refuses (its
    own floor) ends the ladder."""
    n = int(getattr(req, "n_paths"))
    out = []
    for k in range(1, LEVELS + 1):
        paths = n >> k
        if paths < MIN_PATHS:
            break
        try:
            out.append(
                type(req).model_validate(
                    {**req.model_dump(), "n_paths": paths, "seed": req.seed + k}
                )
            )
        except ValueError:
            break
    return out[::-1]


def study(product_id: str, req: BaseModel) -> ConvergenceResponse:
    """Prices `req` at every rung of its ladder, about N paths in all.
    Synchronous: the router
    runs it in the thread pool, under the pricing timeout."""
    if product_id not in MC_PRODUCTS:
        raise NotMonteCarlo(f"{product_id} has no Monte-Carlo engine")
    engine = getattr(req, "engine", "mc")
    if getattr(engine, "value", engine) != "mc":
        raise NotMonteCarlo("the convergence study needs engine='mc'")

    points: List[ConvergencePoint] = []
    for r in ladder(req):
        priced = valuation.price(product_id, r)
        points.append(
            ConvergencePoint(
                paths=r.n_paths,
                seed=r.seed,
                npv=priced.response.npv,
                std_error=priced.response.mc_std_error,
                compute_ms=priced.duration_s * 1000,
            )
        )
    return ConvergenceResponse(n_paths=req.n_paths, points=points)
