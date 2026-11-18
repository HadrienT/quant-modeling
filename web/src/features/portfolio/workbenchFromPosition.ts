import type { Instrument } from "@/shared/api";
import { bookableScript, encodeParams, scriptedKey } from "@/shared/products";

/**
 * The Price page link of a script position (WP 28, lot U2; issue 313, lot
 * b3): the library product with the position's terms (percent terms in %,
 * as the page's form takes them) and its underlying or curve. Null for a
 * position converted from a hand-written pricer (its script is not a
 * library product), or one that has expired.
 */
export function workbenchFromPosition(
	instrument: Instrument | undefined,
	today = new Date().toISOString().slice(0, 10),
): { product: string; p: string } | null {
	const spec = instrument?.spec;
	if (spec?.kind !== "script" || spec.expiry <= today) return null;
	const key = scriptedKey(spec.product);
	const product = bookableScript(key);
	if (!product) return null;
	const values: Record<string, unknown> = {};
	for (const prm of product.params) {
		const v = spec.terms?.[prm.name];
		if (v == null) continue;
		values[prm.name] = prm.unit === "percent" ? +(v * 100).toFixed(6) : v;
	}
	if (spec.underlying) values.tickers = spec.underlying;
	if (product.market === "rates" && product.curve !== "sofr")
		values.currency = spec.currency;
	return { product: key, p: encodeParams(values) };
}
