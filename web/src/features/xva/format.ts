/** Formatting of the xVA page's figures. Money is in the curve's currency. */

const INT = new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 });
const ONE = new Intl.NumberFormat("en-US", {
	minimumFractionDigits: 1,
	maximumFractionDigits: 1,
});

export const dash = "—";

/**
 * A signed amount under the cash-flow convention: what the bank pays or loses
 * is negative, with its minus sign; what it receives is positive, with a plus.
 */
export function signed(v: number | null | undefined): string {
	if (v == null || Number.isNaN(v)) return dash;
	const body = INT.format(Math.abs(v));
	if (Math.round(v) === 0) return "0";
	return v > 0 ? `+${body}` : `−${body}`;
}

/** An unsigned amount: an exposure, a margin, a capital. */
export function amount(v: number | null | undefined): string {
	return v == null || Number.isNaN(v) ? dash : INT.format(v);
}

/** A Monte-Carlo standard error, next to the figure it qualifies. */
export function plusMinus(error: number | null | undefined): string {
	return error == null ? "" : `± ${INT.format(error)}`;
}

/** A sensitivity per basis point of the quote, with one decimal. */
export function perBp(perUnit: number): string {
	const v = perUnit * 1e-4;
	if (Math.abs(v) < 0.05) return "0.0";
	return (v > 0 ? "+" : "−") + ONE.format(Math.abs(v));
}

export function pct(v: number | null | undefined, digits = 2): string {
	return v == null ? dash : `${(v * 100).toFixed(digits)} %`;
}

export function bp(v: number | null | undefined, digits = 0): string {
	return v == null ? dash : `${(v * 1e4).toFixed(digits)} bp`;
}

export const tenorLabel = (years: number) =>
	years < 1
		? `${Math.round(years * 12)}M`
		: `${Math.round(years * 100) / 100}Y`;
