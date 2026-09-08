import { z } from "zod";
import type { EngineKey, ProductDescriptor } from "./types";
import { bsCore, bsCoreDefaults, f, mcCoreDefaults, pct } from "./schema";

/**
 * THE product catalog — dependencies.md §3.4, WP 07 §1.
 * One entry per product; four consumers (pricing workbench, portfolio add,
 * strategy legs, doc cards). It lives in shared/, not features/pricing/.
 *
 * `enabled: false` keeps a product visible-but-greyed with a reason, matching
 * the legacy productRegistry flags (structured / volatility / fx / commodity
 * were switched off because unvetted — and the roadmap says stop adding).
 */

const ANALYTIC = { key: "analytic" as EngineKey, label: "Analytic" };
const MC = {
	key: "mc" as EngineKey,
	label: "Monte-Carlo",
	options: ["paths", "seed", "epsilon"] as const,
};
const MC_PATH = {
	key: "mc" as EngineKey,
	label: "Monte-Carlo",
	options: ["paths", "seed", "steps", "bridge", "antithetic"] as const,
};
const TREE = {
	key: "binomial" as EngineKey,
	label: "Binomial tree",
	options: ["steps"] as const,
};
const PDE = {
	key: "pde" as EngineKey,
	label: "PDE (Crank-Nicolson)",
	options: ["steps"] as const,
};

const UNVETTED =
	"Engine not yet vetted — the roadmap prioritises depth over new payoffs.";

const bump = <T extends object>(v: Record<string, unknown>, extra: T) => ({
	...bsCoreDefaults,
	...mcCoreDefaults,
	...v,
	...extra,
});

export const CATALOG: ProductDescriptor[] = [
	{
		key: "vanilla",
		category: "vanilla",
		label: "European option",
		enabled: true,
		schema: z.object({
			...bsCore,
			engine: z.enum(["analytic", "mc", "binomial", "trinomial", "pde"]),
			n_paths: f.paths(),
			seed: f.seed(),
			tree_steps: z.coerce.number().int().min(10).max(5000),
		}),
		defaults: bump({}, { engine: "analytic", tree_steps: 200 }),
		engines: [
			ANALYTIC,
			MC,
			TREE,
			{ key: "trinomial", label: "Trinomial tree", options: ["steps"] },
			PDE,
		],
		endpoint: "/price/option/vanilla",
		docKey: "option",
		greeks: ["delta", "gamma", "vega", "theta", "rho"],
		toRequest: (v, engine) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate: pct(v.rate),
			dividend: pct(v.dividend),
			vol: pct(v.vol),
			is_call: !!v.is_call,
			engine,
			n_paths: +v.n_paths!,
			seed: +v.seed!,
			tree_steps: +v.tree_steps!,
		}),
	},
	{
		key: "american",
		category: "vanilla",
		label: "American option",
		enabled: true,
		schema: z.object({
			...bsCore,
			engine: z.enum(["binomial", "trinomial"]),
			tree_steps: z.coerce.number().int().min(10).max(5000),
		}),
		defaults: bump({}, { engine: "binomial", tree_steps: 300 }),
		engines: [
			TREE,
			{ key: "trinomial", label: "Trinomial tree", options: ["steps"] },
			PDE,
		],
		endpoint: "/price/option/american-vanilla",
		docKey: "american",
		greeks: ["delta", "gamma", "vega", "theta", "rho"],
		toRequest: (v, engine) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate: pct(v.rate),
			dividend: pct(v.dividend),
			vol: pct(v.vol),
			is_call: !!v.is_call,
			engine,
			tree_steps: +v.tree_steps!,
		}),
	},
	{
		key: "asian",
		category: "exotic",
		label: "Asian option",
		enabled: true,
		schema: z.object({
			...bsCore,
			average_type: z.enum(["arithmetic", "geometric"]),
			engine: z.enum(["analytic", "mc"]),
			n_paths: f.paths(),
			seed: f.seed(),
		}),
		defaults: bump({}, { average_type: "arithmetic", engine: "mc" }),
		engines: [{ ...ANALYTIC, label: "Analytic (geometric)" }, MC],
		endpoint: "/price/option/asian",
		docKey: "asian",
		greeks: ["delta", "gamma", "vega", "theta", "rho"],
		toRequest: (v, engine) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate: pct(v.rate),
			dividend: pct(v.dividend),
			vol: pct(v.vol),
			is_call: !!v.is_call,
			average_type: v.average_type,
			engine,
			n_paths: +v.n_paths!,
			seed: +v.seed!,
		}),
	},
	{
		key: "barrier",
		category: "exotic",
		label: "Barrier option",
		enabled: true,
		schema: z
			.object({
				...bsCore,
				barrier_level: f.strike(),
				barrier_kind: z.enum([
					"up-and-in",
					"up-and-out",
					"down-and-in",
					"down-and-out",
				]),
				rebate: z.coerce.number().min(0),
				n_paths: f.paths(),
				seed: f.seed(),
				brownian_bridge: z.boolean(),
			})
			.refine(
				(d) =>
					d.barrier_kind.startsWith("up")
						? d.barrier_level > d.spot
						: d.barrier_level < d.spot,
				{
					message: "An up-barrier must sit above spot; a down-barrier below.",
					path: ["barrier_level"],
				},
			),
		defaults: bump(
			{ n_paths: 50_000 },
			{
				barrier_level: 120,
				barrier_kind: "up-and-out",
				rebate: 0,
				brownian_bridge: true,
			},
		),
		engines: [MC_PATH],
		endpoint: "/price/option/barrier",
		docKey: "barrier",
		greeks: ["delta", "gamma", "vega", "theta", "rho"],
		toRequest: (v) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate: pct(v.rate),
			dividend: pct(v.dividend),
			vol: pct(v.vol),
			is_call: !!v.is_call,
			barrier_level: +v.barrier_level!,
			barrier_kind: v.barrier_kind,
			rebate: +v.rebate!,
			n_paths: +v.n_paths!,
			seed: +v.seed!,
			brownian_bridge: !!v.brownian_bridge,
		}),
	},
	{
		key: "digital",
		category: "exotic",
		label: "Digital option",
		enabled: true,
		schema: z.object({
			...bsCore,
			payoff_type: z.enum(["cash-or-nothing", "asset-or-nothing"]),
			cash_amount: z.coerce.number().positive(),
		}),
		defaults: bump({}, { payoff_type: "cash-or-nothing", cash_amount: 1 }),
		engines: [ANALYTIC],
		endpoint: "/price/option/digital",
		docKey: "digital",
		greeks: ["delta", "gamma", "vega", "theta", "rho"],
		toRequest: (v) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate: pct(v.rate),
			dividend: pct(v.dividend),
			vol: pct(v.vol),
			is_call: !!v.is_call,
			payoff_type: v.payoff_type,
			cash_amount: +v.cash_amount!,
		}),
	},
	{
		key: "lookback",
		category: "exotic",
		label: "Lookback option",
		enabled: true,
		schema: z.object({
			...bsCore,
			style: z.enum(["fixed-strike", "floating-strike"]),
			extremum: z.enum(["minimum", "maximum"]),
			n_paths: f.paths(),
			seed: f.seed(),
		}),
		defaults: bump({}, { style: "fixed-strike", extremum: "maximum" }),
		engines: [MC_PATH],
		endpoint: "/price/option/lookback",
		docKey: "lookback",
		greeks: ["delta", "gamma", "vega", "theta", "rho"],
		toRequest: (v) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate: pct(v.rate),
			dividend: pct(v.dividend),
			vol: pct(v.vol),
			is_call: !!v.is_call,
			style: v.style,
			extremum: v.extremum,
			n_paths: +v.n_paths!,
			seed: +v.seed!,
		}),
	},
	{
		key: "basket",
		category: "exotic",
		label: "Basket option",
		enabled: true,
		schema: z.object({
			spots: z.string(),
			vols: z.string(),
			weights: z.string(),
			pairwise_correlation: z.coerce.number().min(-0.999).max(0.999),
			strike: f.strike(),
			maturity: f.maturity(),
			rate: f.ratePct(),
			is_call: f.isCall(),
			n_paths: f.paths(),
			seed: f.seed(),
		}),
		defaults: {
			spots: "100, 100, 100",
			vols: "20, 25, 22",
			weights: "0.4, 0.3, 0.3",
			pairwise_correlation: 0.3,
			strike: 100,
			maturity: 1,
			rate: 4,
			is_call: true,
			n_paths: 200_000,
			seed: 1,
		},
		engines: [MC],
		endpoint: "/price/option/basket",
		docKey: "basket",
		greeks: ["delta", "vega"],
		toRequest: (v) => {
			const nums = (s: unknown) =>
				String(s)
					.split(/[,\s]+/)
					.filter(Boolean)
					.map(Number);
			return {
				spots: nums(v.spots),
				vols: nums(v.vols).map((x) => x / 100),
				weights: nums(v.weights),
				pairwise_correlation: +v.pairwise_correlation!,
				strike: +v.strike!,
				maturity: +v.maturity!,
				rate: pct(v.rate),
				is_call: !!v.is_call,
				n_paths: +v.n_paths!,
				seed: +v.seed!,
			};
		},
	},
	{
		key: "rainbow",
		category: "exotic",
		label: "Rainbow (worst/best-of)",
		enabled: true,
		schema: z.object({
			spots: z.string(),
			vols: z.string(),
			pairwise_correlation: z.coerce.number().min(-0.999).max(0.999),
			maturity: f.maturity(),
			strike: z.coerce.number().positive(),
			is_call: f.isCall(),
			rate: f.ratePct(),
			rainbow_kind: z.enum(["worst-of", "best-of"]),
			n_paths: f.paths(),
			seed: f.seed(),
		}),
		defaults: {
			spots: "100, 100",
			vols: "20, 25",
			pairwise_correlation: 0.5,
			maturity: 1,
			strike: 1,
			is_call: true,
			rate: 5,
			rainbow_kind: "worst-of",
			n_paths: 200_000,
			seed: 1,
		},
		engines: [MC],
		endpoint: "/price/option/rainbow",
		docKey: "rainbow",
		greeks: ["delta", "vega"],
		toRequest: (v) => {
			const nums = (s: unknown) =>
				String(s)
					.split(/[,\s]+/)
					.filter(Boolean)
					.map(Number);
			return {
				spots: nums(v.spots),
				vols: nums(v.vols).map((x) => x / 100),
				pairwise_correlation: +v.pairwise_correlation!,
				maturity: +v.maturity!,
				strike: +v.strike!,
				is_call: !!v.is_call,
				rate: pct(v.rate),
				rainbow_kind: v.rainbow_kind,
				n_paths: +v.n_paths!,
				seed: +v.seed!,
			};
		},
	},
	{
		key: "future",
		category: "fixed-income",
		label: "Future",
		enabled: true,
		schema: z.object({
			spot: f.spot(),
			strike: f.strike(),
			maturity: f.maturity(),
			rate: f.ratePct(),
			dividend: f.dividendPct(),
			notional: f.notional(),
		}),
		defaults: {
			spot: 100,
			strike: 100,
			maturity: 1,
			rate: 4,
			dividend: 0,
			notional: 1,
		},
		engines: [ANALYTIC],
		endpoint: "/price/future",
		docKey: "future",
		greeks: ["delta", "rho"],
		toRequest: (v) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate: pct(v.rate),
			dividend: pct(v.dividend),
			notional: +v.notional!,
		}),
	},
	{
		key: "fixed-rate-bond",
		category: "fixed-income",
		label: "Fixed-rate bond",
		enabled: true,
		schema: z.object({
			maturity: f.maturity(),
			rate: f.ratePct(),
			coupon_rate: z.coerce.number().min(0).max(50),
			coupon_frequency: z.coerce.number().int().min(1).max(12),
			notional: f.notional(),
		}),
		defaults: {
			maturity: 5,
			rate: 4,
			coupon_rate: 4,
			coupon_frequency: 2,
			notional: 100,
		},
		engines: [ANALYTIC],
		endpoint: "/price/bond/fixed-rate",
		docKey: "bond",
		greeks: [],
		toRequest: (v) => ({
			maturity: +v.maturity!,
			rate: pct(v.rate),
			coupon_rate: pct(v.coupon_rate),
			coupon_frequency: +v.coupon_frequency!,
			notional: +v.notional!,
		}),
	},
	{
		key: "zero-coupon-bond",
		category: "fixed-income",
		label: "Zero-coupon bond",
		enabled: true,
		schema: z.object({
			maturity: f.maturity(),
			rate: f.ratePct(),
			notional: f.notional(),
		}),
		defaults: { maturity: 5, rate: 4, notional: 100 },
		engines: [ANALYTIC],
		endpoint: "/price/bond/zero-coupon",
		docKey: "bond",
		greeks: [],
		toRequest: (v) => ({
			maturity: +v.maturity!,
			rate: pct(v.rate),
			notional: +v.notional!,
		}),
	},

	// ── unvetted categories: visible, greyed, with a reason ─────────────
	disabled(
		"autocall",
		"structured",
		"Autocall",
		"/price/structured/autocall",
		"autocall",
	),
	disabled(
		"mountain",
		"structured",
		"Mountain / Himalaya",
		"/price/structured/mountain",
		"mountain",
	),
	disabled(
		"variance-swap",
		"volatility",
		"Variance swap",
		"/price/volatility/variance-swap",
		"variance-swap",
	),
	disabled(
		"volatility-swap",
		"volatility",
		"Volatility swap",
		"/price/volatility/volatility-swap",
		"volatility-swap",
	),
	disabled(
		"dispersion-swap",
		"volatility",
		"Dispersion swap",
		"/price/volatility/dispersion-swap",
		"dispersion-swap",
	),
	disabled("fx-forward", "fx", "FX forward", "/price/fx/forward", "fx-forward"),
	disabled("fx-option", "fx", "FX option", "/price/fx/option", "fx-option"),
	disabled(
		"commodity-forward",
		"commodity",
		"Commodity forward",
		"/price/commodity/forward",
		"commodity-forward",
	),
	disabled(
		"commodity-option",
		"commodity",
		"Commodity option",
		"/price/commodity/option",
		"commodity-option",
	),
];

function disabled(
	key: string,
	category: ProductDescriptor["category"],
	label: string,
	endpoint: ProductDescriptor["endpoint"],
	docKey: string,
): ProductDescriptor {
	return {
		key,
		category,
		label,
		enabled: false,
		disabledReason: UNVETTED,
		schema: z.object({}),
		defaults: {},
		engines: [MC],
		endpoint,
		docKey,
		greeks: [],
		toRequest: (v) => v,
	};
}

export const CATALOG_BY_KEY = new Map(CATALOG.map((p) => [p.key, p]));

export const CATEGORY_LABELS: Record<ProductDescriptor["category"], string> = {
	vanilla: "Vanilla",
	exotic: "Exotics",
	"fixed-income": "Fixed income",
	structured: "Structured",
	volatility: "Volatility",
	fx: "FX",
	commodity: "Commodity",
};
