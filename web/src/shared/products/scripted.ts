import { z } from "zod";
import LIBRARY from "./scripted.gen.json";
import { f, pct } from "./schema";
import type {
	EngineKey,
	FieldOverride,
	ProductCategory,
	ProductDescriptor,
	ScriptedProduct,
} from "./types";

/**
 * The products of the script library (api/app/product_library/*.qms, through
 * scripts/gen_product_library.py) as catalog entries. Their form is the term
 * sheet the script declares plus the market: a ticker per underlying and the
 * dynamics (`model`), "auto" by default and every model selectable so that
 * they can be compared. The API renders the script with the terms and prices
 * it (POST /price/scripted-product).
 */
export const SCRIPTED_PRODUCTS = LIBRARY as ScriptedProduct[];

const FAMILY: Record<string, ProductCategory> = {
	"Fixed income": "fixed-income",
	"Vanillas and digitals": "vanilla",
	"Barriers and touch options": "barrier",
	"Asians, lookbacks and ladders": "path",
	"Choosers and compounds": "choice",
	Cliquets: "cliquet",
	"Structured notes": "note",
	Volatility: "volatility",
	"Multi-asset": "multi",
};

/** Products whose reference sheet already exists (shared/products/docs.ts). */
const DOC_KEYS: Record<string, string> = {
	"forward-start-call": "forward-start",
	cliquet: "cliquet",
	napoleon: "napoleon",
	"range-accrual": "corridor",
	// Sheets of products that were greyed out until a script priced them.
	"athena-autocall": "autocall",
	"phoenix-autocall": "autocall",
	himalaya: "mountain",
	"variance-swap": "variance-swap",
	"volatility-swap": "volatility-swap",
	"double-barrier-call": "double-barrier",
	chooser: "chooser",
	compound: "compound",
	"dispersion-swap": "dispersion-swap",
	// The sheets of the hand-written pricers these scripts replaced (issue 313).
	"european-call": "option",
	"european-put": "option",
	"american-put": "american",
	"quanto-call": "quanto",
	"digital-call": "digital",
	"digital-put": "digital",
	"asian-call": "asian",
	"up-and-out-call": "barrier",
	"down-and-in-put": "barrier",
	"fixed-lookback-call": "lookback",
	"floating-lookback-call": "lookback",
	"basket-call": "basket",
	"best-of-call": "rainbow",
	"worst-of-call": "rainbow",
	"zero-coupon-bond": "bond",
	"fixed-rate-bond": "bond",
	"equity-forward": "future",
	"interest-rate-swap": "swap",
	"european-swaption": "swaption",
	"bermudan-swaption": "swaption",
};

/** The government curves a rates product can be priced on: those the
 * database derives (api/app/rates.py; GBP has three par yields only, CHF no
 * free curve). */
export const RATES_CURRENCIES = ["USD", "EUR", "JPY"] as const;

/**
 * Products with a closed form (engine 'analytic', api/app/pricing_service.py
 * ANALYTIC_SCRIPTED): Black-Scholes at the ticker's at-the-money implied vol,
 * the product starting today (its strike in % of today's spot).
 */
export const ANALYTIC_SCRIPTED = new Set([
	"european-call",
	"european-put",
	"digital-call",
	"digital-put",
	"equity-forward",
	// rates products: the cash flows discounted on the curve
	"zero-coupon-bond",
	"fixed-rate-bond",
	"interest-rate-swap",
	// Hull-White in closed form, the model its Monte-Carlo simulates
	"european-swaption",
]);

/**
 * Library products the database cannot price yet, with the reason the page
 * gives: a quanto needs the foreign stock's dividend yield, stored for US
 * listings only (the fix belongs to data-ingest).
 */
const UNAVAILABLE: Record<string, string> = {
	"quanto-call":
		"No dividend yield is stored yet for stocks listed outside the US: the quanto's foreign underlying cannot be priced from the site's database.",
};

export const SINGLE_MODELS = [
	"auto",
	"black_scholes",
	"local_vol",
	"heston",
	"slv",
] as const;
export const MULTI_MODELS = ["auto", "black_scholes"] as const;

/** The dynamics as the page names them. */
const MODEL_LABELS: Record<string, string> = {
	auto: "Auto: the simplest model the product needs",
	black_scholes: "Black-Scholes (the ticker's at-the-money vol)",
	local_vol: "Local volatility (Dupire, the day's smile)",
	heston: "Heston (calibrated to the smile)",
	slv: "Stochastic-local volatility",
};

/** Swap and swaption products: their dates are their coupons' (the API
 * writes the script for the asked length), a swaption's expiry its own. */
const SWAPTIONS = new Set(["european-swaption", "bermudan-swaption"]);
const SWAPS = new Set(["interest-rate-swap", ...SWAPTIONS]);

const DEFAULT_TICKERS = ["AAPL", "MSFT", "GOOGL", "AMZN"];

export const scriptedKey = (slug: string) => `script-${slug}`;

/**
 * The library products a portfolio holds (api/app/portfolio_scripts.book,
 * issue 313 lot b3): one underlying and no quanto -- or a rates product,
 * which reads only its curve. By name.
 */
export const BOOKABLE_SCRIPTS: ScriptedProduct[] = SCRIPTED_PRODUCTS.filter(
	(p) => p.underlyings === 1 && !p.quanto,
).sort((a, b) => a.title.localeCompare(b.title));

/** The bookable product a catalog key names, if any. */
export function bookableScript(key: string): ScriptedProduct | undefined {
	return BOOKABLE_SCRIPTS.find((p) => scriptedKey(p.slug) === key);
}

const term = (unit: string) =>
	unit === "percent"
		? z.coerce.number().min(-100).max(1000)
		: z.coerce.number();

const isTyped = (v: Record<string, unknown>) => v.source === "typed";
const fromMarket = (v: Record<string, unknown>) => v.source !== "typed";

/** One typed underlying of several: spot, vol %, dividend %. */
const TYPED_ASSET = "100,20,0";

/**
 * The market inputs of a product: the database's (a ticker's close, smile
 * and dividend; the government or SOFR curve) or typed by hand -- a
 * fictitious underlying or market, nothing read from the database. Typed,
 * an equity product runs under Black-Scholes at the typed vol (Analytic too
 * where it has a closed form), a rates product under Hull-White on a flat
 * curve with its typed a and sigma.
 */
function typedInputs(
	p: ScriptedProduct,
	shape: Record<string, z.ZodType>,
	defaults: Record<string, unknown>,
	fields: Record<string, FieldOverride>,
) {
	if (p.quanto) return; // its foreign listing and the FX: database only
	const rates = p.market === "rates";
	shape.source = z.enum(["market", "typed"]);
	defaults.source = "market";
	fields.source = {
		label: "Market inputs",
		group: "market",
		optionLabels: rates
			? {
					market:
						p.curve === "sofr"
							? "The SOFR market of the traded swaps"
							: "The government curve",
					typed: "Typed: a flat curve and Hull-White's a, σ",
				}
			: {
					market: "From the database (a ticker)",
					typed: "Typed by hand (a fictitious underlying)",
				},
	};
	const typed = (o: FieldOverride): FieldOverride => ({
		group: "market",
		when: isTyped,
		...o,
	});
	if (rates) {
		shape.flat_rate = f.ratePct();
		defaults.flat_rate = 4;
		fields.flat_rate = typed({ label: "Flat rate", unit: "%", step: 0.25 });
		shape.hw_a = z.coerce.number().min(0.0001).max(1);
		defaults.hw_a = 0.03;
		fields.hw_a = typed({ label: "Mean reversion a", step: 0.01 });
		shape.hw_sigma = z.coerce.number().gt(0).max(1000);
		defaults.hw_sigma = 100;
		fields.hw_sigma = typed({ label: "Short-rate vol σ", unit: "bp", step: 5 });
		if (fields.currency) fields.currency.when = fromMarket;
		return;
	}
	for (const k of ["model", "tickers"])
		if (fields[k]) fields[k] = { ...fields[k], when: fromMarket };
	if (p.underlyings > 1) {
		shape.assets = z.string().regex(/^[\d.\-,;\s]+$/, "Numbers only");
		defaults.assets = Array(p.underlyings).fill(TYPED_ASSET).join(";");
		fields.assets = typed({
			label: "Underlyings, typed",
			widget: "assets",
			count: p.underlyings,
		});
		shape.correlation = z.coerce.number().min(-0.99).max(0.99);
		defaults.correlation = 0.5;
		fields.correlation = typed({
			label: "Correlation, every pair",
			step: 0.05,
		});
		return;
	}
	shape.spot = z.coerce.number().gt(0);
	defaults.spot = 100;
	fields.spot = typed({ label: "Spot", step: 1 });
	shape.vol = z.coerce.number().min(0.01).max(500);
	defaults.vol = 20;
	fields.vol = typed({ label: "Volatility", unit: "%", step: 1 });
	shape.dividend = z.coerce.number().min(-50).max(50);
	defaults.dividend = 0;
	fields.dividend = typed({ label: "Dividend yield", unit: "%", step: 0.25 });
}

const toPrecision = (v: Record<string, unknown>) => v.accuracy === "precision";

/**
 * Price to a precision instead of a path count (issue #331): the engine runs
 * a pilot, picks the paths and says which rule stopped it. 'From the market'
 * needs one ticker's stored chain (its half bid-ask spread in vol).
 */
function precisionInputs(
	shape: Record<string, z.ZodType>,
	defaults: Record<string, unknown>,
	fields: Record<string, FieldOverride>,
	market: boolean,
) {
	shape.accuracy = z.enum(["paths", "precision"]);
	defaults.accuracy = "paths";
	fields.accuracy = {
		label: "Accuracy",
		group: "engine",
		optionLabels: {
			paths: "A number of paths",
			precision: "A precision (the engine picks the paths)",
		},
	};
	fields.n_paths = { ...fields.n_paths, when: (v) => !toPrecision(v) };
	const modes = market
		? (["market", "relative", "absolute"] as const)
		: (["relative", "absolute"] as const);
	shape.precision_mode = z.enum(modes);
	defaults.precision_mode = modes[0];
	fields.precision_mode = {
		label: "Target",
		group: "engine",
		when: toPrecision,
		optionLabels: {
			market: "From the market: k × vega × half bid-ask spread",
			relative: "A share of the price",
			absolute: "In the price's units",
		},
	};
	const when = (mode: string) => (v: Record<string, unknown>) =>
		toPrecision(v) && v.precision_mode === mode;
	shape.precision_k = z.coerce.number().gt(0).max(10);
	defaults.precision_k = 0.25;
	fields.precision_k = {
		label: "k (share of the spread)",
		group: "engine",
		step: 0.05,
		when: when("market"),
	};
	shape.precision_pct = z.coerce.number().gt(0).max(50);
	defaults.precision_pct = 0.1;
	fields.precision_pct = {
		label: "Target error",
		unit: "%",
		group: "engine",
		step: 0.05,
		when: when("relative"),
	};
	shape.precision_abs = z.coerce.number().gt(0);
	defaults.precision_abs = 0.01;
	fields.precision_abs = {
		label: "Target error",
		group: "engine",
		step: 0.01,
		when: when("absolute"),
	};
	shape.time_budget = z.coerce.number().gt(0).max(60);
	defaults.time_budget = 8;
	fields.time_budget = {
		label: "Time budget",
		unit: "s",
		group: "engine",
		step: 1,
		when: toPrecision,
	};
}

/** The request's `precision` block, when the form asks for one. */
export function precisionRequest(v: Record<string, unknown>) {
	if (!toPrecision(v)) return {};
	const mode = String(v.precision_mode);
	const value =
		mode === "market"
			? Number(v.precision_k)
			: mode === "relative"
				? pct(v.precision_pct)
				: Number(v.precision_abs);
	return { precision: { mode, value, time_budget_s: Number(v.time_budget) } };
}

/** Typed underlyings "spot,vol%,div%;..." as the API takes them. */
export function typedAssets(s: string, n: number) {
	return Array.from({ length: n }, (_, i) => {
		const [spot, vol, div] = (s.split(";")[i] ?? TYPED_ASSET)
			.split(",")
			.map((x) => Number(x));
		return { spot, vol: pct(vol), dividend: pct(div ?? 0) };
	});
}

function descriptor(p: ScriptedProduct): ProductDescriptor {
	const multi = p.underlyings > 1;
	const shape: Record<string, z.ZodType> = {};
	const defaults: Record<string, unknown> = {};
	const fields: Record<string, FieldOverride> = {};
	for (const prm of p.params) {
		if (prm.unit === "side") {
			// a payer or a receiver, not a number to type
			shape[prm.name] = z.enum(["1", "-1"]);
			defaults[prm.name] = String(prm.default);
			fields[prm.name] = {
				label: "Side",
				optionLabels:
					p.slug === "interest-rate-swap"
						? {
								"1": "Payer: pays the fixed rate",
								"-1": "Receiver: receives it",
							}
						: { "1": "Payer: the right to pay the strike", "-1": "Receiver" },
			};
			continue;
		}
		shape[prm.name] = term(prm.unit);
		defaults[prm.name] =
			prm.unit === "percent" ? +(prm.default * 100).toFixed(6) : prm.default;
		fields[prm.name] = {
			label: prm.label,
			unit: prm.unit === "percent" ? "%" : undefined,
			step: prm.unit === "percent" ? 0.5 : undefined,
		};
	}
	// The length of the product: its dates stretched from today (the
	// schedules keep their frequency); a swap's in whole years, a
	// swaption's expiry apart.
	const span = Math.max(0.1, +(p.maturity ?? 1).toFixed(2));
	if (SWAPS.has(p.slug)) {
		shape.maturity = z.coerce.number().int().min(1).max(30);
		defaults.maturity = SWAPTIONS.has(p.slug) ? 5 : Math.round(span);
		fields.maturity = {
			label: SWAPTIONS.has(p.slug) ? "Swap length" : "Maturity",
			unit: "years",
			step: 1,
		};
		if (SWAPTIONS.has(p.slug)) {
			shape.expiry = z.coerce.number().min(0.1).max(30);
			defaults.expiry = 1;
			fields.expiry = {
				label: p.slug === "bermudan-swaption" ? "First exercise" : "Expiry",
				unit: "years",
				step: 0.5,
			};
		}
	} else {
		shape.maturity = z.coerce.number().min(0.05).max(30);
		defaults.maturity = span;
		fields.maturity = {
			label: "Maturity",
			unit: "years",
			step: 0.25,
			hint:
				p.underlyings > 0 && !p.market
					? "The product starts today, its levels in % of today's spot; its dates stretch with the maturity."
					: undefined,
		};
	}
	const rates = p.market === "rates";
	const sofr = p.curve === "sofr";
	if (rates && !sofr) {
		// no stock: the currency's government curve, Hull-White fitted to it
		shape.currency = z.enum(RATES_CURRENCIES);
		defaults.currency = "USD";
		fields.currency = { label: "Government curve", group: "market" };
	} else if (!rates) {
		// A quanto prices on `underlyings` (one, in its listing currency),
		// which only correlated Black-Scholes simulates.
		const joint = multi || !!p.quanto;
		shape.model = z.enum(joint ? MULTI_MODELS : SINGLE_MODELS);
		shape.tickers = z
			.string()
			.min(1, "A ticker per underlying")
			.regex(/^[A-Za-z0-9.\-^:, ]+$/, "Tickers separated by commas");
		shape.rate = f.ratePct();
	}
	shape.n_paths = f.paths();
	shape.seed = f.seed();
	precisionInputs(shape, defaults, fields, !multi && !rates && !p.quanto);
	// Several underlyings under local vol differentiate on the CPU only: fewer
	// paths by default, so that the adjoint run fits the API's time limit.
	Object.assign(defaults, { n_paths: multi ? 20_000 : 50_000, seed: 1 });
	if (!rates) {
		Object.assign(defaults, {
			model: "auto",
			tickers: multi
				? DEFAULT_TICKERS.slice(0, p.underlyings).join(", ")
				: p.quanto
					? "SAP.DE"
					: "SPY",
			rate: 4,
		});
		Object.assign(fields, {
			model: {
				label: "Dynamics (model)",
				group: "market",
				optionLabels: MODEL_LABELS,
			},
			tickers: {
				label: multi ? "Underlyings" : "Underlying",
				group: "market",
				widget: multi ? "tickers" : "ticker",
				count: p.underlyings,
			},
		} satisfies Record<string, FieldOverride>);
	}

	typedInputs(p, shape, defaults, fields);

	return {
		key: scriptedKey(p.slug),
		category: FAMILY[p.category] ?? "note",
		label: p.title,
		enabled: !(p.slug in UNAVAILABLE),
		disabledReason: UNAVAILABLE[p.slug],
		schema: z.object(shape),
		defaults,
		engines: [
			...(ANALYTIC_SCRIPTED.has(p.slug)
				? [
						{
							key: "analytic" as EngineKey,
							label: !rates
								? "Analytic (Black-Scholes)"
								: p.slug === "european-swaption"
									? "Analytic (Hull-White closed form)"
									: "Analytic (the curve)",
						},
					]
				: []),
			{
				key: "mc" as EngineKey,
				label: sofr
					? "Monte-Carlo (Hull-White on SOFR)"
					: rates
						? "Monte-Carlo (Hull-White on the curve)"
						: "Monte-Carlo (payoff script)",
				options: ["paths", "seed"],
			},
		],
		endpoint: "/price/scripted-product",
		// the compiled script runs on the GPU (blueprint WP 19, lot G2)
		gpu: true,
		docKey: DOC_KEYS[p.slug],
		greeks: [],
		fields,
		scripted: p,
		toRequest: (v, engine) => ({
			...scriptedRequest(p, v),
			...(engine === "analytic" ? { engine: "analytic" } : {}),
		}),
	};
}

/** The API body for a term sheet in UI units (percent terms in %). */
export function scriptedRequest(
	p: ScriptedProduct,
	v: Record<string, unknown>,
	model?: string,
) {
	const terms = Object.fromEntries(
		p.params.map((prm) => [
			prm.name,
			prm.unit === "percent" ? pct(v[prm.name]) : Number(v[prm.name]),
		]),
	);
	const length = {
		...(v.maturity != null && v.maturity !== ""
			? { maturity: Number(v.maturity) }
			: {}),
		...(v.expiry != null && v.expiry !== ""
			? { expiry: Number(v.expiry) }
			: {}),
	};
	const typed = v.source === "typed";
	if (p.market === "rates")
		// the curve of the currency, or the SOFR market of the traded swaps
		// and swaptions, or a flat typed curve: no ticker, no model to choose
		return {
			product: p.slug,
			terms,
			...length,
			...(typed ? { rate: pct(v.flat_rate) } : {}),
			hull_white: typed
				? {
						curve: "flat",
						mean_reversion: Number(v.hw_a),
						sigma: Number(v.hw_sigma) / 1e4,
					}
				: p.curve === "sofr"
					? { curve: "sofr" }
					: { currency: v.currency ?? "USD" },
			// the risks to the curve and to the short rate's vol, by the tape
			greeks_method: "aad",
			n_paths: Number(v.n_paths),
			seed: Number(v.seed),
			...precisionRequest(v),
		};
	if (typed) {
		// a fictitious underlying: flat Black-Scholes on the typed inputs
		const n = p.underlyings;
		const rho = Number(v.correlation ?? 0);
		const market =
			n > 1
				? {
						underlyings: typedAssets(String(v.assets ?? ""), n),
						correlation: Array.from({ length: n }, (_, i) =>
							Array.from({ length: n }, (_, j) => (i === j ? 1 : rho)),
						),
					}
				: { spot: Number(v.spot), vol: pct(v.vol), dividend: pct(v.dividend) };
		return {
			product: p.slug,
			terms,
			...length,
			model: "black_scholes",
			...market,
			rate: pct(v.rate),
			n_paths: Number(v.n_paths),
			seed: Number(v.seed),
			...precisionRequest(v),
		};
	}
	const tickers = String(v.tickers ?? "")
		.split(/[\s,;]+/)
		.map((t) => t.trim().toUpperCase())
		.filter(Boolean);
	const market =
		p.underlyings > 1
			? { underlyings: tickers.map((ticker) => ({ ticker })) }
			: p.quanto
				? { underlyings: [{ ticker: tickers[0] ?? "" }], currency: p.quanto }
				: { ticker: tickers[0] ?? "" };
	return {
		product: p.slug,
		terms,
		...length,
		model: model ?? v.model,
		...market,
		rate: pct(v.rate),
		n_paths: Number(v.n_paths),
		seed: Number(v.seed),
		...precisionRequest(v),
	};
}

const FAMILY_ORDER = Object.values(FAMILY);

/** By family (the library's order), then by name. */
export const SCRIPTED_CATALOG: ProductDescriptor[] = SCRIPTED_PRODUCTS.map(
	descriptor,
).sort(
	(a, b) =>
		FAMILY_ORDER.indexOf(a.category) - FAMILY_ORDER.indexOf(b.category) ||
		a.label.localeCompare(b.label),
);
