import type { DistributionResponse, SmileOut } from "@/shared/api/types";
import type { FanBand, SmileFitSeries } from "@/shared/viz";

/**
 * Pure helpers of the simulation page's diagnostics (WP 29): what the page
 * derives from the engine's output, each a closed form.
 */

/** Forward F(0, t) of the simulated quantity: S0 e^{(r - q) t}, or the
 * forward itself for SABR (a martingale). */
export function forwardAt(d: DistributionResponse, t: number): number {
	return d.model === "sabr"
		? d.spot
		: d.spot * Math.exp((d.rate - d.dividend) * t);
}

/** Lognormal density of S_t with forward F and vol sigma: Black-Scholes. */
export function lognormalPdf(x: number, F: number, sigma: number, t: number) {
	if (!(x > 0) || !(sigma > 0) || !(t > 0)) return 0;
	const s = sigma * Math.sqrt(t);
	const z = (Math.log(x / F) + 0.5 * s * s) / s;
	return Math.exp(-0.5 * z * z) / (x * s * Math.sqrt(2 * Math.PI));
}

/** Symmetric quantile pairs, outermost first (5-95, then 25-75), and the median. */
export function quantileBands(
	levels: number[],
	q: (number | null)[][],
	label: (lo: number, hi: number) => string = (lo, hi) =>
		`${Math.round(lo * 100)}–${Math.round(hi * 100)}%`,
): { bands: FanBand[]; median: (number | null)[] | null } {
	const bands: FanBand[] = [];
	const order = levels
		.map((l, i) => [l, i] as const)
		.sort((a, b) => a[0] - b[0]);
	for (let k = 0; k < Math.floor(order.length / 2); k++) {
		const [lo, i] = order[k]!;
		const [hi, j] = order[order.length - 1 - k]!;
		if (Math.abs(lo + hi - 1) < 1e-9)
			bands.push({ label: label(lo, hi), lo: q[i]!, hi: q[j]! });
	}
	const m = levels.findIndex((l) => Math.abs(l - 0.5) < 1e-9);
	return { bands, median: m >= 0 ? q[m]! : null };
}

/** The martingale check over every date: the largest |z| = |mean - exact| /
 * stderr, and where. Below ~3.5 over a few dozen dates is what noise gives. */
export function martingaleCheck(d: DistributionResponse) {
	const { mean, stderr, exact } = d.martingale;
	let worst = { z: 0, index: 0 };
	for (let i = 1; i < mean.length; i++) {
		const se = stderr[i]!;
		const z = se > 0 ? Math.abs(mean[i]! - exact[i]!) / se : 0;
		if (z > worst.z) worst = { z, index: i };
	}
	return worst;
}

/** Bin centres of the histogram at one date. */
export function binCentres(lo: number, hi: number, n: number): number[] {
	const w = (hi - lo) / n;
	return Array.from({ length: n }, (_, b) => lo + (b + 0.5) * w);
}

/** The time grid index nearest to t. */
export function nearestIndex(time: number[], t: number): number {
	let best = 0;
	time.forEach((x, i) => {
		if (Math.abs(x - t) < Math.abs(time[best]! - t)) best = i;
	});
	return best;
}

export const pct = (x: number | null | undefined, digits = 1) =>
	x == null ? "—" : `${(x * 100).toFixed(digits)}%`;

/** The last smile the market quotes: the largest gap to it, in vol points,
 * and how many strikes sit inside the model's 95% band. */
export function marketFit(data: DistributionResponse) {
	const smile = [...data.smiles].reverse().find((s) => s.market_iv);
	if (!smile?.market_iv) return null;
	let worst = 0,
		inside = 0,
		n = 0;
	smile.moneyness.forEach((_, j) => {
		const m = smile.model_iv[j],
			k = smile.market_iv![j],
			se = smile.iv_stderr[j];
		if (m == null || k == null || se == null) return;
		n++;
		worst = Math.max(worst, Math.abs(m - k));
		if (Math.abs(m - k) <= 1.96 * se) inside++;
	});
	return n ? { time: smile.time, worst, inside, n } : null;
}

/** The model's smile with its error, and the market's when there is one. */
export function smileSeries(
	smile: SmileOut,
	label = "Model",
	color?: string,
	marketColor?: string,
): SmileFitSeries[] {
	const out: SmileFitSeries[] = [
		{
			label,
			x: smile.moneyness,
			iv: smile.model_iv,
			se: smile.iv_stderr,
			color,
		},
	];
	if (smile.market_iv)
		out.push({
			label: "Market (stored surface)",
			x: smile.moneyness,
			iv: smile.market_iv,
			color: marketColor,
			dashed: true,
		});
	return out;
}
