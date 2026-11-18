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
	"american-put": "american",
	"quanto-call": "quanto",
	"digital-call": "digital",
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
 * the strike fixed on the start date (a forward start).
 */
export const ANALYTIC_SCRIPTED = new Set([
	"european-call",
	"digital-call",
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

function descriptor(p: ScriptedProduct): ProductDescriptor {
	const multi = p.underlyings > 1;
	const shape: Record<string, z.ZodType> = {};
	const defaults: Record<string, unknown> = {};
	const fields: Record<string, FieldOverride> = {};
	for (const prm of p.params) {
		shape[prm.name] = term(prm.unit);
		defaults[prm.name] =
			prm.unit === "percent" ? +(prm.default * 100).toFixed(6) : prm.default;
		fields[prm.name] = {
			label: prm.label,
			unit: prm.unit === "percent" ? "%" : undefined,
			step: prm.unit === "percent" ? 0.5 : undefined,
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
			.regex(/^[A-Za-z.\-^, ]+$/, "Tickers separated by commas");
		shape.rate = f.ratePct();
	}
	shape.n_paths = f.paths();
	shape.seed = f.seed();
	Object.assign(defaults, { n_paths: 50_000, seed: 1 });
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
			model: { label: "Dynamics (model)", group: "market" },
			tickers: {
				label: multi
					? `Tickers, spot(0) to spot(${p.underlyings - 1})`
					: "Ticker",
				group: "market",
			},
		} satisfies Record<string, FieldOverride>);
	}

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
	if (p.market === "rates")
		// the curve of the currency, or the SOFR market of the traded swaps
		// and swaptions: no ticker, no model to choose
		return {
			product: p.slug,
			terms,
			hull_white:
				p.curve === "sofr"
					? { curve: "sofr" }
					: { currency: v.currency ?? "USD" },
			// the risks to the curve and to the short rate's vol, by the tape
			greeks_method: "aad",
			n_paths: Number(v.n_paths),
			seed: Number(v.seed),
		};
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
		model: model ?? v.model,
		...market,
		rate: pct(v.rate),
		n_paths: Number(v.n_paths),
		seed: Number(v.seed),
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
