/** Formatting of the rates page's figures. Inputs are decimals / currency units. */

export const dash = "—";

export function pct(v: number | null | undefined, digits = 3): string {
	return v == null ? dash : `${(v * 100).toFixed(digits)}%`;
}

export function bp(v: number | null | undefined, digits = 1): string {
	return v == null ? dash : `${(v * 1e4).toFixed(digits)} bp`;
}

const MONEY = new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 });

/** Cash-flow sign convention: negative is paid. */
export function money(v: number | null | undefined): string {
	if (v == null) return dash;
	const s = MONEY.format(Math.abs(v));
	// A value that rounds to 0 carries no sign ("−0" would be noise).
	return v < 0 && s !== "0" ? `−${s}` : s;
}

export const tenorLabel = (years: number) =>
	years < 1
		? `${Math.round(years * 12)}M`
		: `${Math.round(years * 100) / 100}Y`;

/** How a column of a quote table is shown and edited. */
export type Unit = "pct" | "bp" | "years";

export const SCALE: Record<Unit, number> = { pct: 100, bp: 1e4, years: 1 };
export const STEP: Record<Unit, string> = {
	pct: "0.01",
	bp: "0.1",
	years: "0.25",
};
export const SUFFIX: Record<Unit, string> = { pct: "%", bp: "bp", years: "y" };
