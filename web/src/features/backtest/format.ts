import type { BacktestMetric } from "@/shared/api";

/** The engine's metric names, in the order the page lists them. A name the
 * page does not know is still shown, after these, under its own name. */
const KNOWN: Record<string, { label: string; percent?: boolean }> = {
	annualized_return: { label: "Annualised return", percent: true },
	annualized_volatility: { label: "Annualised volatility", percent: true },
	sharpe_ratio: { label: "Sharpe ratio" },
	sharpe_ratio_bootstrap: { label: "Sharpe ratio (bootstrap)" },
	sortino_ratio: { label: "Sortino ratio" },
	max_drawdown: { label: "Maximum drawdown", percent: true },
	calmar_ratio: { label: "Calmar ratio" },
	value_at_risk: { label: "Value at risk, one day", percent: true },
	expected_shortfall: { label: "Expected shortfall, one day", percent: true },
	skewness: { label: "Skewness" },
	kurtosis: { label: "Kurtosis" },
	tracking_error: { label: "Tracking error", percent: true },
	information_ratio: { label: "Information ratio" },
	information_ratio_bootstrap: { label: "Information ratio (bootstrap)" },
};

export const metricLabel = (name: string) =>
	KNOWN[name]?.label ??
	name.replace(/_/g, " ").replace(/^./, (c) => c.toUpperCase());

/** Known names first, in the page's order, then the others. */
export function metricOrder(names: string[]): string[] {
	const known = Object.keys(KNOWN).filter((n) => names.includes(n));
	return [...known, ...names.filter((n) => !(n in KNOWN)).sort()];
}

export const ratio = (v: number) => v.toFixed(2).replace("-", "−");
export const percent = (v: number, digits = 1) =>
	`${(v * 100).toFixed(digits)}%`.replace("-", "−");
export const money = (v: number) =>
	new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 }).format(v);

export const formatMetric = (name: string, v: number | null | undefined) =>
	v == null ? "n/a" : KNOWN[name]?.percent ? percent(v) : ratio(v);

/** "[lower, upper]" in the metric's unit, with its confidence level. */
export function interval(
	name: string,
	m: BacktestMetric | null | undefined,
): string {
	if (!m || m.lower == null || m.upper == null) return "no interval";
	return `[${formatMetric(name, m.lower)}, ${formatMetric(name, m.upper)}]`;
}

export const confidence = (m: BacktestMetric | null | undefined) =>
	m?.confidence == null ? "" : `${Math.round(m.confidence * 100)}%`;

/** What a tile says under its value: how the interval was obtained and what
 * the figure means, exactly as the engine words them. */
export const provenance = (m: BacktestMetric) =>
	`Convention: ${m.convention}. Interval: ${m.method}.`;

/** Names shown one by one in the holdings chart; beyond, the rest is one
 * band (the S&P 500 universe holds some 500). */
export const SHOWN = 12;

/** The SHOWN names with the largest average weight, the others summed into
 * "Others": a grouping for the eye, the weights are the engine's. */
export function topHoldings(labels: string[], weights: number[][]) {
	if (labels.length <= SHOWN) return { labels, weights };
	const mean = labels.map(
		(_, j) => weights.reduce((a, row) => a + (row[j] ?? 0), 0) / weights.length,
	);
	const keep = mean
		.map((m, j) => [m, j] as const)
		.sort((a, b) => b[0] - a[0])
		.slice(0, SHOWN)
		.map(([, j]) => j);
	const kept = new Set(keep);
	return {
		labels: [
			...keep.map((j) => labels[j]!),
			`Others (${labels.length - SHOWN})`,
		],
		weights: weights.map((row) => [
			...keep.map((j) => row[j] ?? 0),
			row.reduce((a, w, j) => (kept.has(j) ? a : a + w), 0),
		]),
	};
}
