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
	if (p.market === "rates")
		// the curve of the currency, or the SOFR market of the traded swaps
		// and swaptions: no ticker, no model to choose
		return {
			product: p.slug,
			terms,
			...length,
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
		...length,
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
