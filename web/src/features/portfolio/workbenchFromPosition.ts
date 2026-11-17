import type { Instrument, PositionMark } from "@/shared/api";
import {
	catalogKeyOf,
	encodeParams,
	formValuesFromRequest,
	portfolioDescriptor,
} from "@/shared/products";

const YEAR = 365.25 * 86_400_000;

/**
 * The workbench link of a derivative position (WP 28, lot U2): its contract
 * terms, with the market inputs of its latest mark (same names as the
 * request's: spot, rate, dividend, vol) and the time left to expiry. Null
 * when the product is not in the workbench, the position has expired, or its
 * terms would not come back exactly (see formValuesFromRequest).
 */
export function workbenchFromPosition(
	position: PositionMark,
	instrument: Instrument | undefined,
	today = Date.now(),
): { product: string; p: string } | null {
	const spec = instrument?.spec;
	if (position.kind !== "derivative" || spec?.kind !== "derivative")
		return null;
	const key = catalogKeyOf(spec.product);
	const descriptor = key && portfolioDescriptor(key);
	if (!key || !descriptor) return null;
	const maturity = (Date.parse(spec.expiry) - today) / YEAR;
	if (!(maturity > 0)) return null;
	const request: Record<string, unknown> = { ...spec.params };
	for (const i of position.inputs)
		if (i.name in request && typeof i.value === "number")
			request[i.name] = i.value;
	request.maturity = +maturity.toFixed(4);
	const values = formValuesFromRequest(descriptor, request);
	return values && { product: key, p: encodeParams(values) };
}
