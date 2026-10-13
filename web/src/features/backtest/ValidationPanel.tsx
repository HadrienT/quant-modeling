import { useBacktestValidation } from "@/shared/api";
import type { BacktestValidationRequest } from "@/shared/api";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { DistributionChart, GrowthChart } from "@/shared/viz";
import { Explained, Figure } from "@/shared/xva";
import { percent, ratio } from "./format";
import { INITIAL_CAPITAL, VALIDATION } from "./state";
import { SOURCES, TEX } from "./texts";

const r = (v: number | null | undefined) => (v == null ? "n/a" : ratio(v));

/**
 * Against overfitting: the same universe and costs on a grid of
 * configurations, every one of which counts as a trial. The best of several
 * backtests is biased upwards by the choice itself; the four tests below
 * measure by how much. Asked for when the tab opens: it is a second run.
 */
export function ValidationPanel({
	request,
}: {
	request: BacktestValidationRequest;
}) {
	const result = useBacktestValidation(request);
	const data = result.data;
	if (result.error)
		return <ErrorState error={result.error} onRetry={() => result.refetch()} />;
	if (!data) return <ChartSkeleton />;

	const w = data.walk_forward;
	const offset = data.dates.indexOf(w.first);
	const curve: (number | null)[] = data.dates.map(() => null);
	let value = INITIAL_CAPITAL;
	w.returns.forEach((ret, i) => {
		value *= 1 + (ret ?? 0);
		curve[offset + i] = value;
	});
	const d = data.deflated_sharpe;
	const o = data.overfitting;
	const cv = data.purged_cv;
	const paths = cv.path_sharpes.filter((v): v is number => v != null);

	return (
		<div className="flex flex-col gap-4">
			<GrowthChart
				title="Walk-forward: only what was chosen beforehand"
				subtitle={`On each mark the configuration with the best Sharpe ratio over the previous ${VALIDATION.train} days is chosen, then held for ${VALIDATION.test} days it has not seen. The curve chains those out-of-sample periods, starting from 1,000,000.`}
				dates={data.dates}
				series={[{ label: "Walk-forward, out of sample", values: curve }]}
				marks={w.folds.map((f) => f.test_begin)}
				marksLabel="a configuration is chosen"
				yLabel="USD"
				height={260}
			/>
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface p-3">
				<table className="w-full text-right text-xs">
					<thead>
						<tr className="border-b border-hairline text-ink-muted">
							<th className="py-1 text-left font-medium">Applied from</th>
							<th className="px-2 py-1 font-medium">to</th>
							<th className="px-2 py-1 text-left font-medium">Chosen</th>
							<th className="px-2 py-1 font-medium">Sharpe when chosen</th>
							<th className="px-2 py-1 font-medium">Sharpe afterwards</th>
						</tr>
					</thead>
					<tbody className="font-mono tabular-nums">
						{w.folds.map((f) => (
							<tr key={f.test_begin} className="border-b border-hairline/60">
								<td className="py-1 text-left">{f.test_begin}</td>
								<td className="px-2 py-1">{f.test_end}</td>
								<td
									className="max-w-64 truncate px-2 py-1 text-left font-sans text-ink-secondary"
									title={data.descriptions[f.chosen]}
								>
									#{f.chosen + 1} {data.descriptions[f.chosen]}
								</td>
								<td className="px-2 py-1">{r(f.in_sample_sharpe)}</td>
								<td className="px-2 py-1">{r(f.out_of_sample_sharpe)}</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>

			<Explained
				title="Deflated Sharpe ratio"
				tex={TEX.deflated}
				source={SOURCES.deflated}
				figures={
					<Figure
						label="Probability the best configuration has a positive Sharpe"
						value={
							d.deflated_sharpe_ratio == null
								? "n/a"
								: percent(d.deflated_sharpe_ratio)
						}
						note={`best of ${d.trials}: ${r(d.sharpe)}; expected maximum under no skill: ${r(d.expected_maximum)}`}
					/>
				}
			>
				Among N configurations with no skill at all, the best one still shows a
				positive Sharpe ratio: SR₀, which grows with N and with the dispersion V
				of the ratios tried. The deflated ratio tests the best against that bar
				instead of against zero. N is the {d.trials} configurations of this
				request: every other configuration tried on this universe before
				settling on one raises the bar further, and is not counted here.
			</Explained>

			<DistributionChart
				title="Probability of backtest overfitting: the logits"
				subtitle={`The data are cut into ${VALIDATION.blocks} blocks; for each of the ${o.splits} ways of using half to choose and half to judge, the logit of the rank the chosen configuration reaches on the other half. Below zero: it did worse than the median configuration.`}
				samples={o.logits.filter((v): v is number => v != null)}
				xLabel="Logit of the out-of-sample rank"
				xFormat={(v) => v.toFixed(1)}
				bins={12}
			/>
			<Explained
				title="Probability of backtest overfitting"
				tex={TEX.overfitting}
				source={SOURCES.overfitting}
				figures={
					<Figure
						label="Share of the splits where the choice does worse than the median"
						value={o.probability == null ? "n/a" : percent(o.probability)}
						note={`mean Sharpe when chosen ${r(o.mean_in_sample_sharpe)}, afterwards ${r(o.mean_out_of_sample_sharpe)}`}
					/>
				}
			>
				A frequency over splits that share their data: it has no standard error,
				which is why the whole distribution is drawn above instead of an
				interval. ω̄ is the relative rank, out of sample, of the configuration
				that was best in sample.
			</Explained>

			<Explained
				title="Combinatorial purged cross-validation"
				source={SOURCES.purged}
				figures={
					<Figure
						label={`Sharpe of the ${cv.paths} out-of-sample paths`}
						value={
							paths.length
								? `${ratio(Math.min(...paths))} to ${ratio(Math.max(...paths))}`
								: "n/a"
						}
						note={`${cv.splits} splits of ${VALIDATION.groups} groups, ${VALIDATION.test_groups} held out`}
					/>
				}
			>
				One walk-forward is one path through history. Holding out{" "}
				{VALIDATION.test_groups} of {VALIDATION.groups} groups in every possible
				way gives {cv.paths} complete out-of-sample paths, with{" "}
				{VALIDATION.purge} days purged around each test group and{" "}
				{VALIDATION.embargo} embargoed after it so that overlapping windows do
				not leak. The paths share their data: they are not independent draws,
				and their range is not a confidence interval.
			</Explained>
		</div>
	);
}
