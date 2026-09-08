import type { z } from "zod";

/**
 * Derive renderable field metadata from a product's zod schema (WP 07 §2).
 * Only the selected product's fields exist — changing product remounts the form.
 */

export type FieldMeta = {
	name: string;
	kind: "number" | "boolean" | "enum" | "text";
	options?: string[];
	label: string;
	/** unit shown in the field: %, y, … */
	unit?: string;
	min?: number;
	max?: number;
	step?: number;
	group: "contract" | "market" | "engine";
};

const LABELS: Record<string, string> = {
	spot: "Spot",
	strike: "Strike",
	maturity: "Maturity",
	rate: "Risk-free rate",
	dividend: "Dividend yield",
	vol: "Volatility",
	is_call: "Call",
	notional: "Notional",
	n_paths: "Paths",
	seed: "Seed",
	tree_steps: "Tree steps",
	barrier_level: "Barrier level",
	barrier_kind: "Barrier kind",
	rebate: "Rebate",
	brownian_bridge: "Brownian bridge",
	average_type: "Average type",
	payoff_type: "Payoff type",
	cash_amount: "Cash amount",
	style: "Style",
	extremum: "Extremum",
	spots: "Spots",
	vols: "Volatilities",
	weights: "Weights",
	pairwise_correlation: "Pairwise correlation",
	coupon_rate: "Coupon rate",
	coupon_frequency: "Coupon frequency / yr",
	rainbow_kind: "Rainbow kind",
	engine: "Engine",
};

const UNITS: Record<string, string> = {
	rate: "%",
	dividend: "%",
	vol: "%",
	coupon_rate: "%",
	maturity: "y",
};

const MARKET = new Set(["spot", "rate", "dividend", "vol", "spots", "vols"]);
const ENGINE = new Set([
	"engine",
	"n_paths",
	"seed",
	"tree_steps",
	"brownian_bridge",
]);

type AnyDef = {
	typeName?: string;
	schema?: unknown;
	innerType?: unknown;
	values?: string[] | Record<string, string>;
	checks?: { kind: string; value: number }[];
};

function defOf(node: unknown): AnyDef {
	let cur = node;
	for (let i = 0; i < 8; i++) {
		const d = (cur as { _def?: AnyDef })?._def;
		if (!d) return {};
		if (d.schema) cur = d.schema;
		else if (d.innerType) cur = d.innerType;
		else return d;
	}
	return (cur as { _def?: AnyDef })?._def ?? {};
}

export function fieldsFromSchema(schema: z.ZodType): FieldMeta[] {
	let node: unknown = schema;
	for (let i = 0; i < 8; i++) {
		const d = (node as { _def?: AnyDef })?._def;
		if (d?.schema) node = d.schema;
		else if (d?.innerType) node = d.innerType;
		else break;
	}
	const shape = (node as z.ZodObject<z.ZodRawShape> | undefined)?.shape;
	if (!shape) return [];

	return Object.entries(shape).map(([name, raw]) => {
		const def = defOf(raw);
		let kind: FieldMeta["kind"] = "text";
		let options: string[] | undefined;

		if (def.typeName === "ZodNumber") kind = "number";
		else if (def.typeName === "ZodBoolean") kind = "boolean";
		else if (def.typeName === "ZodEnum") {
			kind = "enum";
			options = Array.isArray(def.values)
				? def.values
				: Object.values(def.values ?? {});
		} else if (def.typeName === "ZodString") kind = "text";

		const checks = def.checks ?? [];
		const min = checks.find((c) => c.kind === "min")?.value;
		const max = checks.find((c) => c.kind === "max")?.value;

		return {
			name,
			kind,
			options,
			label: LABELS[name] ?? name,
			unit: UNITS[name],
			min,
			max,
			step:
				UNITS[name] === "%"
					? 0.25
					: name === "seed" || name === "n_paths"
						? 1
						: undefined,
			group: ENGINE.has(name)
				? "engine"
				: MARKET.has(name)
					? "market"
					: "contract",
		};
	});
}
