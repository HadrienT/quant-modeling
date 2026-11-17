import type { BacktestHedging } from "@/shared/api";
import { DensityChart, useChartTheme } from "@/shared/viz";
import { SOURCES, hedgeLabel, withInterval } from "./hedging";

const TERMS = ["delta", "gamma", "theta", "vega", "unexplained"] as const;
/** Hedges whose option is marked alike explain its change alike: one row. */
function explanationRows(data: BacktestHedging) {
	const rows: {
		hedges: string[];
		explanation: BacktestHedging["hedges"][number]["explanation"];
	}[] = [];
	for (const h of data.hedges) {
		const key = JSON.stringify(TERMS.map((t) => h.explanation[t]?.value));
		const row = rows.find(
			(r) => JSON.stringify(TERMS.map((t) => r.explanation[t]?.value)) === key,
		);
		if (row) row.hedges.push(h.hedge);
		else rows.push({ hedges: [h.hedge], explanation: h.explanation });
	}
	return rows;
}

const num = (v: number | null | undefined, digits = 2) =>
	v == null ? "n/a" : v.toFixed(digits).replace("-", "−");

/**
 * What each hedge leaves: the P&L at maturity of an option bought and
 * delta-hedged, path by path, on the same paths for every hedge. Every mean
 * and standard deviation carries its 95 % Monte-Carlo interval (the true
 * model's delta often beats the corrected ones by less than that, so the
 * intervals are the point), next to Derman and Kamal's floor: the noise of
 * hedging only so many times, which no delta removes.
 */
export function HedgingResults({ data }: { data: BacktestHedging }) {
	const t = useChartTheme();
	const lo = data.bins[0]!;
	const hi = data.bins[data.bins.length - 1]!;
	const paths = data.hedges[0]!.histogram.reduce((a, b) => a + b, 0);
	const w = data.world;
	const params =
		w.model === "heston"
			? `v0 ${num(w.v0 as number, 4)}, kappa ${num(w.kappa as number)}, theta ${num(w.theta as number, 4)}, xi ${num(w.xi as number)}, rho ${num(w.rho as number)}`
			: `vol ${num(100 * (w.volatility as number), 1)}%`;
	const uncalibrated = data.hedges.filter((h) => h.uncalibrated_steps > 0);

	return (
		<div className="flex flex-col gap-4">
			<p className="text-xs text-ink-secondary">
				{data.data.source}
				{data.data.snapshot ? ` (snapshot ${data.data.snapshot})` : ""}: spot{" "}
				{num(w.spot as number)}, rate {num(100 * (w.rate as number))}%, dividend{" "}
				{num(100 * (w.dividend_yield as number))}%, {params}.{" "}
				{data.is_call ? "Call" : "Put"} struck at {num(data.strike)},{" "}
				{data.maturity} years, premium {num(data.premium, 3)}.
				{data.coefficients &&
					` Minimum-variance delta: a ${num(data.coefficients.a, 4)}, b ${num(data.coefficients.b, 4)}, c ${num(data.coefficients.c, 4)}, fitted on another seed.`}
			</p>
			<DensityChart
				title="P&L at maturity, per hedge"
				subtitle="Option bought and delta-hedged, in currency; the same paths for every hedge"
				histograms={data.hedges.map((h, i) => ({
					label: hedgeLabel(h.hedge),
					lo,
					hi,
					counts: h.histogram,
					total: paths,
					color: t.series[i % t.series.length],
					outline: true,
				}))}
				markers={[{ label: "Zero", x: 0 }]}
				xLabel="P&L"
				xFormat={(v) => v.toFixed(1)}
				height={300}
			/>
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface p-3">
				<table className="w-full text-right text-xs">
					<thead className="text-ink-muted">
						<tr className="border-b border-hairline">
							<th className="py-1.5 pr-3 text-left font-medium">Hedge</th>
							<th className="px-2 font-medium">Mean</th>
							<th className="px-2 font-medium">Std deviation</th>
							<th className="px-2 font-medium">Skew</th>
							<th className="px-2 font-medium">Kurtosis</th>
							<th className="px-2 font-medium">1% / 99%</th>
							<th className="px-2 font-medium">Derman-Kamal floor</th>
						</tr>
					</thead>
					<tbody className="font-mono">
						{data.hedges.map((h) => (
							<tr key={h.hedge} className="border-b border-hairline/60">
								<td className="py-1.5 pr-3 text-left font-sans text-ink-secondary">
									{hedgeLabel(h.hedge)}
								</td>
								<td className="px-2" title={h.mean.convention}>
									{withInterval(h.mean)}
								</td>
								<td className="px-2" title={h.standard_deviation.convention}>
									{withInterval(h.standard_deviation)}
								</td>
								<td className="px-2">{num(h.skewness)}</td>
								<td className="px-2">{num(h.kurtosis)}</td>
								<td className="px-2">
									{num(h.quantiles["0.01"])} / {num(h.quantiles["0.99"])}
								</td>
								<td className="px-2">{num(h.discretisation_noise, 3)}</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface p-3">
				<h3 className="mb-1 text-sm font-medium text-ink">
					The option's value change, explained
				</h3>
				<table className="w-full text-right text-xs">
					<thead className="text-ink-muted">
						<tr className="border-b border-hairline">
							<th className="py-1.5 pr-3 text-left font-medium">
								Marked as in
							</th>
							{TERMS.map((term) => (
								<th key={term} className="px-2 font-medium capitalize">
									{term}
								</th>
							))}
						</tr>
					</thead>
					<tbody className="font-mono">
						{explanationRows(data).map(({ hedges, explanation }) => (
							<tr key={hedges.join()} className="border-b border-hairline/60">
								<td className="py-1.5 pr-3 text-left font-sans text-ink-secondary">
									{hedges.map(hedgeLabel).join(", ")}
								</td>
								{TERMS.map((term) => (
									<td
										key={term}
										className="px-2"
										title={explanation[term]?.convention}
									>
										{withInterval(explanation[term])}
									</td>
								))}
							</tr>
						))}
					</tbody>
				</table>
				<p className="mt-2 text-2xs text-ink-muted">
					The option's Black-Scholes greeks at the previous date, at the
					volatility it is marked at; the five terms add up to the change of its
					value. It depends on how the option is marked, not on the hedge:
					hedges that mark it alike share a row. Means over the paths, each ±
					its 95% Monte-Carlo half-width; hover a figure for its convention.
				</p>
			</div>
			<ul className="list-disc pl-5 text-xs text-ink-secondary">
				<li>
					The hedges share their paths: a difference of standard deviations
					smaller than their intervals is not a difference.
				</li>
				{uncalibrated.length > 0 && (
					<li>
						Dates where no implied volatility could be read (the option worth
						its intrinsic value, to the pricer's accuracy) hedge it as the
						forward it has become, or not at all, and mark it at the last
						volatility read:{" "}
						{uncalibrated
							.map(
								(h) =>
									`${hedgeLabel(h.hedge)} ${h.uncalibrated_steps.toLocaleString("en-US")}`,
							)
							.join(", ")}{" "}
						path-dates. Not an error.
					</li>
				)}
				{data.hedges.map((h) => (
					<li key={h.hedge}>
						{hedgeLabel(h.hedge)}: {h.convention}.
					</li>
				))}
			</ul>
			<p className="text-2xs text-ink-muted">
				{SOURCES.dermanKamal} {SOURCES.derman} {SOURCES.hullWhite}{" "}
				{SOURCES.heston}
			</p>
		</div>
	);
}
