import type {
	XvaPortfolioId,
	XvaRating,
	XvaRequest,
	XvaSensitivitiesRequest,
} from "@/shared/api";

/** What the page's URL holds: a view is a link. */
export type XvaView = {
	portfolio: XvaPortfolioId;
	counterparty: XvaRating;
	own: XvaRating;
	/** No collateral, variation margin, or variation and initial margin. */
	csa: "none" | "vm" | "im";
	/** The wrong-way parameter of the chosen scenario. */
	wwr: number;
	/** Funding spread on both sides, in basis points. */
	funding: number;
	paths: number;
};

export const DEFAULT_VIEW: XvaView = {
	portfolio: "single_swap",
	counterparty: "BBB",
	own: "A",
	csa: "none",
	wwr: 0,
	funding: 50,
	paths: 5000,
};

/** Recovery of both parties: the market convention for senior debt. */
export const RECOVERY = 0.4;
/** One seed: the same scenarios whatever the terms, so that two views differ
 * by their terms and not by their noise. */
export const SEED = 42;

export const PATHS = [2000, 5000, 10000, 20000] as const;
export const FUNDING_BP = [0, 25, 50, 100] as const;

/** The portfolios the page offers; `custom` needs scripts of its own. */
export const isOffered = (id: string) => id !== "custom";

/**
 * The collateral agreement of the page: the terms the margin rules for
 * non-cleared derivatives require — zero thresholds, daily calls, a margin
 * period of risk of ten business days — with the flows of that period
 * withheld, and initial margin on both sides when asked for.
 */
function csa(v: XvaView): XvaRequest["csa"] {
	if (v.csa === "none") return null;
	return {
		threshold_counterparty: 0,
		threshold_bank: 0,
		minimum_transfer_amount: 0,
		margin_period_of_risk_days: 10,
		cashflows: "withheld",
		initial_margin: v.csa === "im",
		initial_margin_model: "auto",
		collateral_rate_spread: 0,
	};
}

export function nettingSetRequest(v: XvaView): XvaRequest {
	return {
		portfolio: v.portfolio,
		counterparty_rating: v.counterparty,
		own_rating: v.own,
		csa: csa(v),
		recovery: RECOVERY,
		borrowing_spread: v.funding * 1e-4,
		lending_spread: v.funding * 1e-4,
		wrong_way_risk: v.wwr,
		paths: v.paths,
		seed: SEED,
		pfe_confidence: 0.95,
		device: "auto",
	};
}

export function sensitivitiesRequest(v: XvaView): XvaSensitivitiesRequest {
	return {
		portfolio: v.portfolio,
		counterparty_rating: v.counterparty,
		own_rating: v.own,
		csa: csa(v),
		recovery: RECOVERY,
		borrowing_spread: v.funding * 1e-4,
		lending_spread: v.funding * 1e-4,
		sector: "financial",
		paths: Math.min(v.paths, 20000),
		seed: SEED,
	};
}
