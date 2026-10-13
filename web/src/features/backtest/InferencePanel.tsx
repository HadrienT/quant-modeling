import type { BacktestStrategy } from "@/shared/api";
import { IntervalChart } from "@/shared/viz";
import { Explained, Figure } from "@/shared/xva";
import { confidence, interval, percent, ratio } from "./format";
import { SOURCES, TEX } from "./texts";

const p = (v: number | null | undefined) =>
	v == null ? "n/a" : v < 0.001 ? "< 0.001" : v.toFixed(3);

/**
 * What the data can and cannot tell apart: the Sharpe ratios with their
 * intervals, the test of the strategy against equal weights, the Sharpe ratio
 * the optimiser saw against the one that was realised, and the exposure to
 * the Fama-French factors.
 */
export function InferencePanel({ data }: { data: BacktestStrategy }) {
	const runs = [
		{ label: "Strategy, net", run: data.strategy },
		...(data.gross ? [{ label: "Strategy, gross", run: data.gross }] : []),
		{ label: "Equal weight", run: data.equal_weight },
		{ label: "Buy and hold", run: data.buy_and_hold },
		{ label: data.benchmark_ticker, run: data.benchmark },
	];
	const sharpe = data.strategy.report.metrics.sharpe_ratio;
	const d = data.sharpe_difference;
	const f = data.factor_regression;
	const inSample = data.allocations.in_sample_sharpe.filter(
		(v): v is number => v != null,
	);
	const report = data.strategy.report;

	return (
		<div className="flex flex-col gap-4">
			<IntervalChart
				title="Sharpe ratios and their confidence intervals"
				subtitle={
					sharpe
						? `${confidence(sharpe)} intervals. ${sharpe.method}.`
						: undefined
				}
				rows={runs.map(({ label, run }) => {
					const m = run.report.metrics.sharpe_ratio;
					return {
						label,
						value: m?.value ?? null,
						lower: m?.lower ?? null,
						upper: m?.upper ?? null,
					};
				})}
				format={ratio}
				xLabel="Annualised Sharpe ratio"
			/>

			<Explained
				title="How precisely is a Sharpe ratio known?"
				tex={TEX.sharpe}
				source={`${SOURCES.lo} ${SOURCES.psr}`}
				figures={
					<Figure
						label="Probability that the true ratio is above zero"
						value={
							report.probabilistic_sharpe_ratio == null
								? "n/a"
								: percent(report.probabilistic_sharpe_ratio)
						}
						note={`t-statistic ${
							report.t_statistic == null ? "n/a" : ratio(report.t_statistic)
						}; track record needed: ${
							report.minimum_track_record_years == null
								? "n/a"
								: `${report.minimum_track_record_years.toFixed(1)} years`
						}`}
					/>
				}
			>
				Much less than its two decimals suggest. Its standard error shrinks with
				the square root of the number of periods T and grows with negative
				skewness and fat tails; {report.years.toFixed(1)} years of daily returns
				leave the interval drawn above. sr is the ratio per period, P the number
				of periods in a year.
			</Explained>

			<Explained
				title="Is the strategy better than equal weights?"
				source={`${SOURCES.memmel} ${SOURCES.bootstrap} ${SOURCES.naive}`}
				figures={
					<div className="flex flex-wrap gap-2">
						<Figure
							label="Sharpe difference, analytical"
							value={
								d.analytical.value == null ? "n/a" : ratio(d.analytical.value)
							}
							note={`${interval("sharpe_ratio", d.analytical)}, p = ${p(d.p_value)}`}
						/>
						{d.bootstrap && (
							<Figure
								label="Sharpe difference, bootstrap"
								value={
									d.bootstrap.value == null ? "n/a" : ratio(d.bootstrap.value)
								}
								note={`${interval("sharpe_ratio", d.bootstrap)}, p = ${p(d.bootstrap_p_value)}`}
							/>
						)}
					</div>
				}
			>
				An optimised portfolio has to beat the portfolio that needs no estimate
				at all, and on a few years of data it rarely does so significantly: a
				p-value above 0.05 means the data do not separate the two. The two
				series move together (correlation{" "}
				{d.correlation == null ? "n/a" : ratio(d.correlation)}), which is what
				makes the test sharper than comparing the two intervals above.
				Convention: {d.analytical.convention}. Analytical interval:{" "}
				{d.analytical.method}.
				{d.bootstrap && ` Bootstrap interval: ${d.bootstrap.method}.`}
			</Explained>

			<Explained
				title="The Sharpe ratio the optimiser saw"
				source={SOURCES.shrinkage}
				figures={
					<Figure
						label="In-sample, range over the decisions"
						value={
							inSample.length
								? `${ratio(Math.min(...inSample))} to ${ratio(Math.max(...inSample))}`
								: "n/a"
						}
						note={`realised: ${sharpe?.value == null ? "n/a" : ratio(sharpe.value)}`}
					/>
				}
			>
				At each decision the engine reports the Sharpe ratio of the chosen
				weights on the window that chose them. It is measured on the very data
				the weights were fitted to, not a forecast: set against the realised
				ratio, it shows how little the estimation window says about what
				follows.
			</Explained>

			<Explained
				title={`Exposure to the ${f.factors.length} Fama-French factors`}
				tex={TEX.factors}
				source={SOURCES.factors}
				figures={
					<Figure
						label="Alpha, a year"
						value={f.alpha.value == null ? "n/a" : percent(f.alpha.value)}
						note={`${interval("annualized_return", f.alpha)}, t = ${
							f.alpha_t_statistic == null ? "n/a" : ratio(f.alpha_t_statistic)
						}`}
					/>
				}
			>
				Convention: {f.alpha.convention}. Interval: {f.alpha.method}. On{" "}
				{f.observations} days from {f.first} to {f.last}: the factor library is
				published with a lag, so the regression stops before the run does. R² ={" "}
				{f.r_squared == null ? "n/a" : percent(f.r_squared)}. Betas:{" "}
				{f.factors
					.map(
						(name, i) =>
							`${name} ${
								f.betas[i]?.value == null ? "n/a" : ratio(f.betas[i]!.value!)
							} ${interval("sharpe_ratio", f.betas[i])}`,
					)
					.join("; ")}
				.
			</Explained>
		</div>
	);
}
