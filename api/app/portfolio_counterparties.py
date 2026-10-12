"""Counterparties and netting sets of a portfolio — who its over-the-counter
trades face, and under which agreement (blueprint/wp/23-xva.md, lot X11).

A portfolio carries its own counterparties (portfolio_schemas.Counterparty),
so they follow it wherever it is kept: in the browser of an anonymous
visitor, on the server, in an exported file. This module holds the examples
offered on the Portfolio page: fictional names, one per kind of counterparty
a derivatives desk faces, with the collateral terms that kind usually has.
"""

from __future__ import annotations

from typing import List, Optional, Tuple

from .portfolio_schemas import Counterparty, NettingSet, Portfolio
from .xva_schemas import CsaInput

#: Minimum transfer amount of the margin rules for non-cleared derivatives
#: (BCBS-IOSCO: EUR 500 000), in currency units.
_MTA = 500_000.0

#: Variation margin as the margin rules require it: no threshold, daily calls.
_VARIATION_MARGIN = CsaInput(minimum_transfer_amount=_MTA)

EXAMPLES: Tuple[Counterparty, ...] = (
    Counterparty(
        id="example-dealer",
        name="Northgate Bank (dealer)",
        rating="A",
        sector="financial",
        netting_sets=[
            NettingSet(
                id="example-dealer-vm",
                name="ISDA with CSA: variation margin",
                csa=_VARIATION_MARGIN,
            ),
            NettingSet(
                id="example-dealer-im",
                name="ISDA with CSA: variation and initial margin",
                csa=_VARIATION_MARGIN.model_copy(update={"initial_margin": True}),
            ),
        ],
    ),
    Counterparty(
        id="example-fund",
        name="Halden Capital (hedge fund)",
        rating="BB",
        sector="financial",
        netting_sets=[
            NettingSet(
                id="example-fund-vm",
                name="ISDA with CSA: variation margin",
                csa=_VARIATION_MARGIN,
            )
        ],
    ),
    Counterparty(
        id="example-corporate",
        name="Meridian Industries (corporate)",
        rating="BBB",
        sector="basic_materials_energy_industrials",
        netting_sets=[
            # A non-financial corporate hedging its business is outside the
            # margin rules and usually posts nothing.
            NettingSet(id="example-corporate-isda", name="ISDA, uncollateralised")
        ],
    ),
    Counterparty(
        id="example-high-yield",
        name="Calder Shipping (high-yield corporate)",
        rating="B",
        sector="consumer_transport_administrative",
        netting_sets=[
            NettingSet(id="example-high-yield-isda", name="ISDA, uncollateralised")
        ],
    ),
    Counterparty(
        id="example-sovereign",
        name="Kingdom of Arvelia (sovereign)",
        rating="AA",
        sector="sovereign",
        netting_sets=[
            NettingSet(id="example-sovereign-isda", name="ISDA, uncollateralised")
        ],
    ),
)


def examples() -> List[Counterparty]:
    return [c.model_copy(deep=True) for c in EXAMPLES]


def netting_set(
    portfolio: Portfolio, netting_set_id: str
) -> Optional[Tuple[Counterparty, NettingSet]]:
    """A netting set of the portfolio with the counterparty it belongs to."""
    for c in portfolio.counterparties:
        for n in c.netting_sets:
            if n.id == netting_set_id:
                return c, n
    return None


__all__ = ["EXAMPLES", "examples", "netting_set"]
