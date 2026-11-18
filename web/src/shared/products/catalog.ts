import { z } from "zod";
import type { EngineKey, ProductDescriptor } from "./types";
import { bsCore, bsCoreDefaults, f, mcCoreDefaults, pct } from "./schema";
import { SCRIPTED_CATALOG } from "./scripted";

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

const UNVETTED =
	"No market data for this asset class in the site's database yet: its reference sheet is here, its pricing is not.";

const bump = <T extends object>(v: Record<string, unknown>, extra: T) => ({
	...bsCoreDefaults,
	...mcCoreDefaults,
	...v,
	...extra,
});

const HAND_WRITTEN: ProductDescriptor[] = [
	{
		key: "vanilla",
		category: "vanilla",
		label: "European option",
		enabled: true,
		schema: z.object({
			...bsCore,
			engine: z.enum(["analytic", "mc"]),
			// Up to the API's cap: CPU and GPU only separate at tens of
			// millions of paths, below that launch overhead dominates.
			n_paths: z.coerce.number().int().min(1_000).max(100_000_000),
			seed: f.seed(),
		}),
		defaults: bump({}, { engine: "analytic" }),
		gpu: true,
		// The site offers Analytic and Monte-Carlo only (issue 313): the trees and
		// the PDE stay in the library and its tests, as oracles.
		engines: [ANALYTIC, MC],
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
		}),
	},
	{
		// A model, not a new payoff: a vanilla option on a foreign asset, paid
		// in the domestic currency at a fixed rate (QuantoBlackScholesModel).
		key: "quanto",
		category: "fx",
		label: "Quanto option",
		enabled: true,
		schema: z.object({
			spot: f.spot(),
			strike: f.strike(),
			maturity: f.maturity(),
			rate_domestic: f.ratePct(),
			rate_foreign: f.ratePct(),
			dividend: f.dividendPct(),
			vol: f.volPct(),
			fx_vol: f.volPct(),
			correlation: z.coerce.number().min(-1).max(1),
			fx_rate: z.coerce.number().positive(),
			is_call: f.isCall(),
			engine: z.enum(["analytic", "mc"]),
			n_paths: f.paths(),
			seed: f.seed(),
		}),
		defaults: {
			...mcCoreDefaults,
			spot: 100,
			strike: 100,
			maturity: 1,
			rate_domestic: 4.5,
			rate_foreign: 3,
			dividend: 0,
			vol: 20,
			fx_vol: 8,
			correlation: 0.3,
			fx_rate: 1,
			is_call: true,
			engine: "analytic",
		},
		engines: [ANALYTIC, MC],
		endpoint: "/price/option/quanto",
		docKey: "quanto",
		greeks: ["delta", "gamma", "vega", "theta", "rho"],
		toRequest: (v, engine) => ({
			spot: +v.spot!,
			strike: +v.strike!,
			maturity: +v.maturity!,
			rate_domestic: pct(v.rate_domestic),
			rate_foreign: pct(v.rate_foreign),
			dividend: pct(v.dividend),
			vol: pct(v.vol),
			fx_vol: pct(v.fx_vol),
			correlation: +v.correlation!,
			fx_rate: +v.fx_rate!,
			is_call: !!v.is_call,
			engine,
			n_paths: +v.n_paths!,
			seed: +v.seed!,
		}),
	},
	{
		key: "american",
		category: "vanilla",
		label: "American option",
		enabled: true,
		schema: z.object({
			...bsCore,
			engine: z.enum(["mc", "binomial"]),
			n_paths: f.paths(),
			seed: f.seed(),
		}),
		defaults: bump({ n_paths: 100_000 }, { engine: "mc" }),
		// Longstaff-Schwartz on a script, a Bermudan on 50 dates a year (issue 313).
		engines: [
			{
				...MC,
				label: "Monte-Carlo (Longstaff-Schwartz)",
				options: ["paths", "seed"],
			},
		],
		// A portfolio marks every day of its history: the tree, still in the
		// API, keeps that history fast.
		markEngine: "binomial",
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
			...(engine === "mc"
				? { n_paths: +v.n_paths!, seed: +v.seed! }
				: { tree_steps: 300 }),
		}),
	},
	{
		key: "asian",
		category: "path",
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
		category: "barrier",
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
		category: "vanilla",
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
		category: "path",
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
		category: "multi",
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
		category: "multi",
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
	rates("swap", "Interest rate swap", "/price/rates/swap"),
	rates("swaption", "Swaption", "/price/rates/swaption"),

	// ── no market data for these yet: in the Products reference only ────
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

/**
 * A rates product (see ProductDescriptor.rates). The form values only say
 * which quote set and what was edited; the request is built from them and the
 * set by features/pricing/rates, not by `toRequest`.
 */
function rates(
	key: "swap" | "swaption",
	label: string,
	endpoint: ProductDescriptor["endpoint"],
): ProductDescriptor {
	return {
		key,
		category: "fixed-income",
		label,
		enabled: true,
		schema: z.object({
			quote_set: z.enum(["usd-sofr", "eur-illustrative"]),
			quotes: z.unknown().optional(),
			contract: z.unknown().optional(),
		}),
		defaults: { quote_set: "usd-sofr" },
		engines: [ANALYTIC],
		endpoint,
		docKey: key,
		greeks: [],
		rates: key,
		toRequest: (v) => v,
	};
}

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

/** The order of the families in every product list. */
const FAMILY_ORDER: ProductDescriptor["category"][] = [
	"vanilla",
	"barrier",
	"path",
	"choice",
	"cliquet",
	"note",
	"volatility",
	"multi",
	"fx",
	"commodity",
	"fixed-income",
];

/**
 * Every product by family, whether a hand-written pricer or a script of the
 * library: the list says what a product is, not how it is priced. Within a
 * family the hand-written pricers come first (their engines and the GPU),
 * then the scripts by name. A product without a pricing (`enabled: false`)
 * shows in the Products reference only.
 */
/**
 * The hand-written equity pricers: no longer on the site (issue 313, the
 * scripts of the library are the catalog), still the forms a portfolio books
 * and marks until it books scripts (lot b3). The library's trees, closed
 * forms and engines behind them stay as oracles.
 */
const PORTFOLIO_ONLY = new Set([
	"vanilla",
	"quanto",
	"american",
	"asian",
	"barrier",
	"digital",
	"lookback",
	"basket",
	"rainbow",
]);

/**
 * Every product by family: the scripts of the library, and the rates
 * products the language does not write yet (lot b2). A product without a
 * pricing (`enabled: false`) shows in the Products reference only.
 */
export const CATALOG: ProductDescriptor[] = [
	...HAND_WRITTEN.filter((p) => !PORTFOLIO_ONLY.has(p.key)),
	...SCRIPTED_CATALOG,
].sort(
	(a, b) => FAMILY_ORDER.indexOf(a.category) - FAMILY_ORDER.indexOf(b.category),
);

/** Where the workbench and the products page open: the European call, by its
 * closed form, instant. */
export const DEFAULT_PRODUCT_KEY = "script-european-call";

/**
 * Products that were greyed out until a script of the library priced them:
 * their old links open the script.
 */
const FORMER_KEYS: Record<string, string> = {
	// The hand-written pricers the scripts replaced on the site (issue 313).
	vanilla: "script-european-call",
	american: "script-american-put",
	quanto: "script-quanto-call",
	asian: "script-asian-call",
	barrier: "script-up-and-out-call",
	digital: "script-digital-call",
	lookback: "script-fixed-lookback-call",
	basket: "script-basket-call",
	rainbow: "script-worst-of-call",
	autocall: "script-athena-autocall",
	mountain: "script-himalaya",
	"variance-swap": "script-variance-swap",
	"volatility-swap": "script-volatility-swap",
	"dispersion-swap": "script-dispersion-swap",
	"double-barrier": "script-double-barrier-call",
	chooser: "script-chooser",
	compound: "script-compound",
};

/** The catalog key a URL means: a former key opens its script. */
export function resolveProductKey(key: string | undefined): string | undefined {
	return key ? (FORMER_KEYS[key] ?? key) : undefined;
}

/** Every descriptor by key: the catalog, and the portfolio's own forms. */
export const CATALOG_BY_KEY = new Map(
	[...CATALOG, ...HAND_WRITTEN.filter((p) => PORTFOLIO_ONLY.has(p.key))].map(
		(p) => [p.key, p],
	),
);

export const CATEGORY_LABELS: Record<ProductDescriptor["category"], string> = {
	vanilla: "Vanillas & digitals",
	barrier: "Barriers & touch",
	path: "Asians, lookbacks & ladders",
	choice: "Choosers & compounds",
	cliquet: "Cliquets",
	note: "Structured notes",
	volatility: "Volatility",
	multi: "Multi-asset",
	fx: "FX",
	commodity: "Commodities",
	"fixed-income": "Fixed income",
};
