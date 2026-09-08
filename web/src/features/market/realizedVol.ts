/** Realised (close-to-close) annualised volatility over a trailing window. */
export function realizedVol(
	closes: number[],
	window: number,
	periodsPerYear = 252,
): number | null {
	if (closes.length < window + 1) return null;
	const slice = closes.slice(-window - 1);
	const rets: number[] = [];
	for (let i = 1; i < slice.length; i++) {
		rets.push(Math.log(slice[i]! / slice[i - 1]!));
	}
	const mean = rets.reduce((a, b) => a + b, 0) / rets.length;
	const variance =
		rets.reduce((a, b) => a + (b - mean) ** 2, 0) / (rets.length - 1);
	return Math.sqrt(variance * periodsPerYear);
}
