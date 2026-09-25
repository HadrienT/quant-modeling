import type { z } from "zod";
import type { PricingPath } from "@/shared/api/pricing";

export type ProductCategory =
	| "vanilla"
	| "exotic"
	| "fixed-income"
	| "structured"
	| "volatility"
	| "fx"
	| "commodity"
	// Products of the script library (api/app/product_library), by family.
	| "script-vanilla"
	| "script-barrier"
	| "script-path"
	| "script-cliquet"
	| "script-note"
	| "script-volatility"
	| "script-multi";

/** A product of the script library, as scripted.gen.json describes it. */
export type ScriptedProduct = {
	slug: string;
	title: string;
	category: string;
	underlyings: number;
	sources: string[];
	summary: string;
	params: {
		name: string;
		default: number;
		unit: "percent" | "amount" | "number";
		label: string;
	}[];
	script: string;
};

/** Per-field overrides of what zodFields derives from a schema. */
export type FieldOverride = {
	label?: string;
	unit?: string;
	group?: "contract" | "market" | "engine";
	step?: number;
};

export type EngineKey = "analytic" | "mc" | "binomial" | "trinomial" | "pde";

export type GreekName = "delta" | "gamma" | "vega" | "theta" | "rho";

export type EngineOption =
	"paths" | "seed" | "steps" | "bridge" | "antithetic" | "epsilon";

export type EngineCapability = {
	key: EngineKey;
	label: string;
	/** MC-only knobs surface under the engine when selected (WP 07 §3). */
	options?: readonly EngineOption[];
};

export type ProductDescriptor = {
	key: string;
	category: ProductCategory;
	label: string;
	/** false = shown greyed with `disabledReason` on hover (WP 07 §1). */
	enabled: boolean;
	disabledReason?: string;
	/** zod schema: fields, bounds, defaults. Drives the whole form (WP 07 §2). */
	schema: z.ZodType;
	/** default values, in UI units (vol/rate as %). */
	defaults: Record<string, unknown>;
	engines: EngineCapability[];
	/**
	 * Pricing route, typed from the OpenAPI paths — undefined for a
	 * doc-only catalog entry with no backend at all yet (a product that
	 * exists on the Products reference page but was never wired past the
	 * roadmap's "stop adding payoffs" line). `enabled` is always false
	 * when this is undefined, so ResultsPanel never has to call it.
	 */
	endpoint?: PricingPath;
	/** → productDocs (WP 99). */
	docKey?: string;
	greeks: GreekName[];
	/** labels / units / groups for fields zodFields cannot name itself. */
	fields?: Record<string, FieldOverride>;
	/** set for a product of the script library: its template. */
	scripted?: ScriptedProduct;
	/** map UI-unit form values to the API request body (units converted at the edge). */
	toRequest: (values: Record<string, unknown>, engine: EngineKey) => unknown;
};
