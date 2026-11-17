import { CATALOG_BY_KEY } from "./catalog";
import type { ProductDescriptor } from "./types";

/**
 * The workbench products a portfolio can hold, as catalog key → the pricing
 * registry's product id (api/app/valuation.py). Single-underlying products
 * only: each day's mark takes the underlying's close as spot. Shared so the
 * workbench can offer "Add to portfolio" and a position can open the
 * workbench (WP 28, lot U2) without one feature importing the other.
 */
export const PORTFOLIO_PRODUCTS: Record<string, string> = {
	vanilla: "vanilla",
	american: "american_vanilla",
	quanto: "quanto",
	asian: "asian",
	barrier: "barrier",
	digital: "digital",
	lookback: "lookback",
	future: "future",
};

/** The catalog key of a registry product id, if the workbench has it. */
export function catalogKeyOf(productId: string): string | undefined {
	return Object.keys(PORTFOLIO_PRODUCTS).find(
		(k) => PORTFOLIO_PRODUCTS[k] === productId,
	);
}

const NUMERIC_TOLERANCE = 1e-9;

/**
 * The workbench form values of a request, the inverse of `toRequest`. Which
 * fields the form keeps in percent is not written anywhere: it is read off
 * `toRequest` itself, by probing each numeric field. The result is checked by
 * a round trip, and refused (null) when any shared field would not come back
 * to the same number: a link that opens other terms than the position's is
 * worse than no link.
 */
export function formValuesFromRequest(
	descriptor: ProductDescriptor,
	request: Record<string, unknown>,
): Record<string, unknown> | null {
	const engine = descriptor.engines[0]?.key ?? "analytic";
	const req = (v: Record<string, unknown>) =>
		descriptor.toRequest(v, engine) as Record<string, unknown>;
	const values: Record<string, unknown> = { ...descriptor.defaults };
	for (const [k, d] of Object.entries(descriptor.defaults)) {
		if (!(k in request)) continue;
		const v = request[k];
		if (typeof d === "number" && typeof v === "number") {
			const probe = req({ ...descriptor.defaults, [k]: 7 })[k];
			const percent =
				typeof probe === "number" && Math.abs(probe - 0.07) < 1e-12;
			values[k] = percent ? +(v * 100).toPrecision(12) : v;
		} else values[k] = v;
	}
	const back = req(values);
	for (const [k, v] of Object.entries(request)) {
		if (typeof v !== "number" || typeof back[k] !== "number") continue;
		const err = Math.abs((back[k] as number) - v);
		if (err > NUMERIC_TOLERANCE * Math.max(1, Math.abs(v))) return null;
	}
	return values;
}

/** Shorthand for the descriptor of a portfolio product. */
export function portfolioDescriptor(
	key: string,
): ProductDescriptor | undefined {
	return key in PORTFOLIO_PRODUCTS ? CATALOG_BY_KEY.get(key) : undefined;
}
