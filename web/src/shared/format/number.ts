/**
 * Number / currency / percent / date formatting — blueprint WP 01 §6.
 * Pure functions, unit-tested. The UI never calls toLocaleString directly.
 */

export type Magnitude =
	| "price" // 4 decimals
	| "vol" // volatility points, 2 decimals, shown as %
	| "rate" // 2 decimals, shown as %
	| "greek" // adaptive
	| "notional" // compact
	| "integer"
	| "plain";

const PRICE = new Intl.NumberFormat("en-US", {
	minimumFractionDigits: 4,
	maximumFractionDigits: 4,
});
const TWO = new Intl.NumberFormat("en-US", {
	minimumFractionDigits: 2,
	maximumFractionDigits: 2,
});
const COMPACT = new Intl.NumberFormat("en-US", {
	notation: "compact",
	maximumFractionDigits: 2,
});
const INT = new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 });

/** Format a value for display. Returns "—" for null / undefined / NaN. */
export function formatNumber(
	value: number | null | undefined,
	magnitude: Magnitude = "plain",
): string {
	if (value == null || Number.isNaN(value)) return "—";
	switch (magnitude) {
		case "price":
			return PRICE.format(value);
		case "vol":
		case "rate":
			return `${TWO.format(value * 100)}%`;
		case "greek":
			return formatGreek(value);
		case "notional":
			return COMPACT.format(value);
		case "integer":
			return INT.format(value);
		case "plain":
			return TWO.format(value);
	}
}

function formatGreek(value: number): string {
	const abs = Math.abs(value);
	if (abs !== 0 && abs < 1e-3) return value.toExponential(2);
	if (abs >= 1e4) return COMPACT.format(value);
	return new Intl.NumberFormat("en-US", { maximumFractionDigits: 4 }).format(
		value,
	);
}

/** Signed string with an explicit + for positives (P&L rule, WP 01 §3). */
export function formatSigned(
	value: number | null | undefined,
	magnitude: Magnitude = "plain",
): string {
	if (value == null || Number.isNaN(value)) return "—";
	const body = formatNumber(Math.abs(value), magnitude);
	if (value > 0) return `+${body}`;
	if (value < 0) return `−${body}`;
	return body;
}
