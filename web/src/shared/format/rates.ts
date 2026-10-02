/**
 * Formatting of rates figures (curves, swaps, swaptions). Inputs are
 * decimals (0.0123 = 1.23 % = 123 bp), money in currency units, times in
 * years.
 */

const DASH = "—";

/** A rate in percent, to the tenth of a basis point. */
export function ratePct(v: number | null | undefined, digits = 3): string {
	return v == null ? DASH : `${(v * 100).toFixed(digits)}%`;
}

/** A rate or a normal vol in basis points. */
export function rateBp(v: number | null | undefined, digits = 1): string {
	return v == null ? DASH : `${(v * 1e4).toFixed(digits)} bp`;
}

const MONEY = new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 });

/** Whole currency units, cash-flow sign convention: negative is paid. */
export function signedMoney(v: number | null | undefined): string {
	if (v == null) return DASH;
	const s = MONEY.format(Math.abs(v));
	// A value that rounds to 0 carries no sign ("−0" would be noise).
	return v < 0 && s !== "0" ? `−${s}` : s;
}

export const tenorLabel = (years: number) =>
	years < 1
		? `${Math.round(years * 12)}M`
		: `${Math.round(years * 100) / 100}Y`;
