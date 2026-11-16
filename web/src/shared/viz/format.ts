/** Axis / value formatters per magnitude — blueprint WP 06 §3. */
import { scaleLinear } from "@visx/scale";

export const fmt = {
	price: (v: number) => v.toFixed(2),
	pct: (v: number) => `${(v * 100).toFixed(1)}%`,
	volPts: (v: number) => `${(v * 100).toFixed(1)}`,
	signedPct: (v: number) =>
		`${v >= 0 ? "+" : "−"}${Math.abs(v * 100).toFixed(1)}%`,
	compact: (v: number) =>
		new Intl.NumberFormat("en-US", {
			notation: "compact",
			maximumFractionDigits: 1,
		}).format(v),
	years: (v: number) => (v < 1 ? `${Math.round(v * 12)}m` : `${v}y`),
	index100: (v: number) => v.toFixed(1),
} as const;

/** "Nice" domain with a little headroom, keeping zero when the sign matters.
 *
 * `clipOutliers` swaps the true min/max for the 5th/95th percentile before
 * padding -- for a handful of quantities (risk reversal / butterfly across
 * maturities) a single degenerate point (an illiquid near-zero-DTE slice
 * whose delta-bucket solve barely converges) can be an order of magnitude
 * outside the rest of the series, and a plain min/max domain then crushes
 * every legitimate value flat against the zero line, same failure mode
 * `robustZExtent` fixed for the 3D surface. Off by default: most callers
 * (a smile, a Greek profile) want the true extent, where "the line touches
 * the edge" is itself informative. Needs at least 5 points to be meaningful;
 * below that it falls back to the plain extent. */
export function niceDomain(
	values: number[],
	{
		includeZero = false,
		clipOutliers = false,
	}: { includeZero?: boolean; clipOutliers?: boolean } = {},
): [number, number] {
	let min: number;
	let max: number;
	if (clipOutliers && values.length >= 5) {
		const sorted = [...values].sort((a, b) => a - b);
		const at = (q: number) =>
			sorted[
				Math.min(
					sorted.length - 1,
					Math.max(0, Math.round(q * (sorted.length - 1))),
				)
			]!;
		min = at(0.05);
		max = at(0.95);
		if (min === max) {
			min = Math.min(...values);
			max = Math.max(...values);
		}
	} else {
		min = Math.min(...values);
		max = Math.max(...values);
	}
	if (includeZero) {
		min = Math.min(min, 0);
		max = Math.max(max, 0);
	}
	if (min === max) {
		const pad = Math.abs(min) || 1;
		return [min - pad, max + pad];
	}
	const pad = (max - min) * 0.06;
	return [min - pad, max + pad];
}

const MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split(" ");

/** An ISO date as a year with a fraction, so that ticks fall on years. */
export function yearFraction(iso: string): number {
	const year = Number(iso.slice(0, 4));
	const start = Date.UTC(year, 0, 1);
	const end = Date.UTC(year + 1, 0, 1);
	return year + (Date.parse(iso) - start) / (end - start);
}

/** A year with a fraction as a tick label: the year, or the month in it. */
export function tickDate(v: number): string {
	const year = Math.floor(v + 1e-9);
	const month = Math.round((v - year) * 12);
	if (month === 0) return String(year);
	if (month === 12) return String(year + 1);
	return `${MONTHS[month]} ${year}`;
}

/**
 * A tick formatter for a linear axis: every label gets the decimals its tick
 * step needs, no more, no fewer -- so a gamma axis reads 0.000, 0.005, 0.010
 * instead of 0e+0, 5e-3, 0.01, 0.01 (two ticks rounded to one label).
 * `numTicks` must be the count the axis asks its scale for.
 */
export function stepFormat(
	domain: [number, number],
	numTicks: number,
): (v: number) => string {
	const ticks = scaleLinear({ domain }).ticks(numTicks);
	const step = ticks.length > 1 ? Math.abs(ticks[1]! - ticks[0]!) : 0;
	const decimals =
		step > 0
			? Math.min(12, Math.max(0, Math.ceil(-Math.log10(step) - 1e-9)))
			: 2;
	return (v) => (Math.abs(v) < step / 2 ? 0 : v).toFixed(decimals);
}

/**
 * Ticks for a log axis at 1, 2 and 5 times each power of ten inside the
 * domain, thinned to 1 and 10 per decade when the domain spans many decades:
 * a log scale's own ticks put nine labels per decade on top of each other.
 */
export function logTicks([lo, hi]: [number, number]): number[] {
	if (!(lo > 0) || !(hi > lo)) return [];
	const decades = Math.log10(hi / lo);
	const mantissas = decades > 3 ? [1] : decades > 1.5 ? [1, 3] : [1, 2, 5];
	const out: number[] = [];
	for (let e = Math.floor(Math.log10(lo)); e <= Math.ceil(Math.log10(hi)); e++)
		for (const m of mantissas) {
			const v = m * 10 ** e;
			if (v >= lo * (1 - 1e-9) && v <= hi * (1 + 1e-9)) out.push(v);
		}
	// A domain narrower than one 1-2-5 step: label its two ends instead.
	return out.length >= 2 ? out : [lo, hi];
}
