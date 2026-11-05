/** One measured point of a convergence study: a pricing at `paths` paths. */
export type MeasuredPoint = {
	paths: number;
	seed?: number;
	npv: number;
	std_error: number;
	compute_ms?: number | null;
};

export type SlopeFit = { slope: number; stdError: number } | null;

/**
 * Least-squares slope of ln(std error) on ln(paths) and its regression
 * standard error, over the points with a non-zero error. Null with fewer
 * than three: two points fix a line, they do not measure its uncertainty.
 */
export function fitSlope(points: MeasuredPoint[]): SlopeFit {
	const xy = points
		.filter((p) => p.std_error > 0 && p.paths > 0)
		.map((p) => [Math.log(p.paths), Math.log(p.std_error)] as const);
	const n = xy.length;
	if (n < 3) return null;
	const mx = xy.reduce((s, [x]) => s + x, 0) / n;
	const my = xy.reduce((s, [, y]) => s + y, 0) / n;
	const sxx = xy.reduce((s, [x]) => s + (x - mx) ** 2, 0);
	if (sxx === 0) return null;
	const slope = xy.reduce((s, [x, y]) => s + (x - mx) * (y - my), 0) / sxx;
	const rss = xy.reduce((s, [x, y]) => s + (y - my - slope * (x - mx)) ** 2, 0);
	return { slope, stdError: Math.sqrt(rss / (n - 2) / sxx) };
}

/** "slope −0.503 ± 0.005 · consistent with 1/√N", read at two standard
 *  errors of the fit. */
export function slopeLine(fit: SlopeFit): string {
	if (fit === null)
		return "log–log · too few points with a non-zero error to fit a slope";
	const { slope, stdError: se } = fit;
	const fmt = (x: number) => x.toFixed(3).replace("-", "−");
	const band = Math.max(2 * se, 0.01);
	const verdict =
		Math.abs(slope + 0.5) <= band
			? "consistent with 1/√N"
			: slope < -0.5
				? "faster than 1/√N"
				: "slower than 1/√N: the error estimate may not be reliable";
	return `log–log · slope ${fmt(slope)} ± ${se.toFixed(3)} · ${verdict}`;
}
