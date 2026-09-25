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
	"Vanillas and digitals": "script-vanilla",
	"Barriers and touch options": "script-barrier",
	"Asians, lookbacks and ladders": "script-path",
	Cliquets: "script-cliquet",
	"Structured notes": "script-note",
	Volatility: "script-volatility",
	"Multi-asset": "script-multi",
};

/** Products whose reference sheet already exists (shared/products/docs.ts). */
const DOC_KEYS: Record<string, string> = {
	"forward-start-call": "forward-start",
	cliquet: "cliquet",
	napoleon: "napoleon",
	"range-accrual": "corridor",
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
	shape.model = z.enum(multi ? MULTI_MODELS : SINGLE_MODELS);
	shape.tickers = z
		.string()
		.min(1, "A ticker per underlying")
		.regex(/^[A-Za-z.\-^, ]+$/, "Tickers separated by commas");
	shape.rate = f.ratePct();
	shape.n_paths = f.paths();
	shape.seed = f.seed();
	Object.assign(defaults, {
		model: "auto",
		tickers: multi ? DEFAULT_TICKERS.slice(0, p.underlyings).join(", ") : "SPY",
		rate: 4,
		n_paths: 50_000,
		seed: 1,
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

	return {
		key: scriptedKey(p.slug),
		category: FAMILY[p.category] ?? "script-note",
		label: p.title,
		enabled: true,
		schema: z.object(shape),
		defaults,
		engines: [
			{
				key: "mc" as EngineKey,
				label: "Monte-Carlo (payoff script)",
				options: ["paths", "seed"],
			},
		],
		endpoint: "/price/scripted-product",
		docKey: DOC_KEYS[p.slug],
		greeks: [],
		fields,
		scripted: p,
		toRequest: (v) => scriptedRequest(p, v),
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
	const tickers = String(v.tickers ?? "")
		.split(/[\s,;]+/)
		.map((t) => t.trim().toUpperCase())
		.filter(Boolean);
	const market =
		p.underlyings > 1
			? { underlyings: tickers.map((ticker) => ({ ticker })) }
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

export const SCRIPTED_CATALOG: ProductDescriptor[] =
	SCRIPTED_PRODUCTS.map(descriptor);
