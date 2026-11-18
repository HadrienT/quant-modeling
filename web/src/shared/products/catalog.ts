import { z } from "zod";
import type { EngineKey, ProductDescriptor } from "./types";
import { SCRIPTED_CATALOG } from "./scripted";

/**
 * THE product catalog — dependencies.md §3.4, WP 07 §1.
 * One entry per product; its consumers are the pricing workbench, the
 * portfolio's trade form (through the library's scripts) and the doc cards. It lives in shared/, not features/pricing/.
 *
 * `enabled: false` keeps a product visible-but-greyed with a reason, matching
 * the legacy productRegistry flags (structured / volatility / fx / commodity
 * were switched off because unvetted — and the roadmap says stop adding).
 */

const MC = {
	key: "mc" as EngineKey,
	label: "Monte-Carlo",
	options: ["paths", "seed", "epsilon"] as const,
};

const UNVETTED =
	"No market data for this asset class in the site's database yet: its reference sheet is here, its pricing is not.";

/** The asset classes the database has no market data for: their reference
 * sheets, no pricing. */
const HAND_WRITTEN: ProductDescriptor[] = [
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
 * Every product by family: the scripts of the library (issue 313: the
 * hand-written pricers left the site in lots b1 to b3 and stay in the
 * library, the API and the tests as oracles). A product without a pricing
 * (`enabled: false`) shows in the Products reference only.
 */
export const CATALOG: ProductDescriptor[] = [
	...HAND_WRITTEN,
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
	future: "script-equity-forward",
	"fixed-rate-bond": "script-fixed-rate-bond",
	"zero-coupon-bond": "script-zero-coupon-bond",
	swap: "script-interest-rate-swap",
	swaption: "script-european-swaption",
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

/** Every descriptor by key. */
export const CATALOG_BY_KEY = new Map(CATALOG.map((p) => [p.key, p]));

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
