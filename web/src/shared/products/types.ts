import type { z } from "zod";
import type { PricingPath } from "@/shared/api/pricing";

export type ProductCategory =
	| "vanilla"
	| "exotic"
	| "fixed-income"
	| "structured"
	| "volatility"
	| "fx"
	| "commodity";

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
	/** pricing route, typed from the OpenAPI paths. */
	endpoint: PricingPath;
	/** → productDocs (WP 99). */
	docKey?: string;
	greeks: GreekName[];
	/** map UI-unit form values to the API request body (units converted at the edge). */
	toRequest: (values: Record<string, unknown>, engine: EngineKey) => unknown;
};
