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
	/** The agreement's terms: the same threshold on both sides, the minimum
	 * transfer amount (currency units), the margin period of risk (business
	 * days), and what happens to the trades' flows during it. */
	threshold: number;
	mta: number;
	mpor: number;
	flows: "withheld" | "paid" | "only_bank_pays";
	/** A rate script from the Scripting page: the netting set of `custom`. */
	script: string | null;
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
	threshold: 0,
	mta: 0,
	mpor: 10,
	flows: "withheld",
	script: null,
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
/** Terms offered on the page, for books of a 10 million notional. */
export const THRESHOLDS = [0, 100_000, 250_000, 1_000_000] as const;
export const MTAS = [0, 50_000, 250_000] as const;
export const MPORS = [5, 10, 20] as const;
export const FUNDING_BP = [0, 25, 50, 100] as const;

/** The portfolios the page offers; `custom` only with a script of one's own
 * (sent from the Scripting page). */
export const isOffered = (id: string, script: string | null) =>
	id !== "custom" || script != null;

/** One's own script as the netting set of `custom`. */
const scripts = (v: XvaView) =>
	v.portfolio === "custom" && v.script
		? [{ script: v.script, quantity: 1, label: "Your script" }]
		: [];

/**
 * The collateral agreement of the page. By default the terms the margin rules
 * for non-cleared derivatives require — zero thresholds, daily calls, a margin
 * period of risk of ten business days — with the flows of that period
 * withheld; every term can be changed, and initial margin added on both
 * sides.
 */
function csa(v: XvaView): XvaRequest["csa"] {
	if (v.csa === "none") return null;
	return {
		threshold_counterparty: v.threshold,
		threshold_bank: v.threshold,
		minimum_transfer_amount: v.mta,
		margin_period_of_risk_days: v.mpor,
		cashflows: v.flows,
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
		scripts: scripts(v),
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
