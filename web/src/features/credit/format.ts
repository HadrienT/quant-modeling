/** Formatting of the credit page's figures. Inputs are decimals / dollars. */

const COMPACT = new Intl.NumberFormat("en-US", {
	notation: "compact",
	maximumFractionDigits: 2,
});
const TWO = new Intl.NumberFormat("en-US", {
	minimumFractionDigits: 2,
	maximumFractionDigits: 2,
});

export const dash = "—";

export function bp(v: number | null | undefined, digits = 1): string {
	return v == null ? dash : `${(v * 1e4).toFixed(digits)} bp`;
}

export function pct(v: number | null | undefined, digits = 2): string {
	return v == null ? dash : `${(v * 100).toFixed(digits)}%`;
}

export function usd(v: number | null | undefined): string {
	if (v == null) return dash;
	const s = `$${COMPACT.format(Math.abs(v))}`;
	return v < 0 ? `−${s}` : s;
}

/** A count, compact: 14.59B. */
export function count(v: number | null | undefined): string {
	return v == null ? dash : COMPACT.format(v);
}

/** A statement figure by the unit the API gives it. */
export function figure(v: number, unit: string, key: string): string {
	switch (unit) {
		case "USD":
			return usd(v);
		case "USD/shares":
			return `${v < 0 ? "−" : ""}$${TWO.format(Math.abs(v))}`;
		case "shares":
			return COMPACT.format(v);
		case "ratio":
			return key.endsWith("margin") ? pct(v, 1) : `${TWO.format(v)}×`;
		default:
			return TWO.format(v);
	}
}

export const tenorLabel = (years: number) =>
	years < 1 ? `${Math.round(years * 12)}M` : `${years}Y`;
