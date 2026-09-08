import { z } from "zod";

/**
 * Reusable field fragments — vol and rate are entered in PERCENT in the UI and
 * converted to decimals at the edge (WP 07 §2). Bounds live here, once.
 */

export const f = {
	spot: () => z.coerce.number().positive("Spot must be positive"),
	strike: () => z.coerce.number().positive("Strike must be positive"),
	maturity: () =>
		z.coerce.number().positive("Maturity must be positive").max(50),
	ratePct: () => z.coerce.number().min(-20).max(50),
	dividendPct: () => z.coerce.number().min(-20).max(50),
	volPct: () =>
		z.coerce.number().positive("Volatility must be positive").max(500),
	notional: () => z.coerce.number().positive(),
	isCall: () => z.boolean(),
	seed: () => z.coerce.number().int().min(0),
	paths: () => z.coerce.number().int().min(1_000).max(2_000_000),
};

/** decimal ← percent at the API boundary. */
export const pct = (v: unknown) => Number(v) / 100;

export const bsCore = {
	spot: f.spot(),
	strike: f.strike(),
	maturity: f.maturity(),
	rate: f.ratePct(),
	dividend: f.dividendPct(),
	vol: f.volPct(),
	is_call: f.isCall(),
};

export const bsCoreDefaults = {
	spot: 100,
	strike: 100,
	maturity: 1,
	rate: 4,
	dividend: 0,
	vol: 20,
	is_call: true,
};

export const mcCore = {
	n_paths: f.paths(),
	seed: f.seed(),
};

export const mcCoreDefaults = { n_paths: 200_000, seed: 1 };
