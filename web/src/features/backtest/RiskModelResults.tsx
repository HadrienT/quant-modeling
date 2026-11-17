import type { BacktestRiskModel } from "@/shared/api";
import { Metric, MetricRow } from "@/shared/ui/density";
import { GrowthChart } from "@/shared/viz";

const p = (v: number | null | undefined) => (v == null ? "n/a" : v.toFixed(3));

const SOURCES = {
	kupiec:
		"Kupiec, P. (1995). Techniques for verifying the accuracy of risk measurement models. Journal of Derivatives 3(2).",
	christoffersen:
		"Christoffersen, P. (1998). Evaluating interval forecasts. International Economic Review 39(4).",
	basel:
		"Basel Committee on Banking Supervision (1996). Supervisory framework for the use of backtesting in conjunction with the internal models approach to market risk capital requirements.",
} as const;

/**
 * Was the VaR right? Each day the hypothetical P&L (the holdings of the day
 * before, held still) against the VaR announced the evening before; the
 * days it was exceeded marked on the curve; then the wheel's tests, each
 * with what its p-value can and cannot say.
 */
export function RiskModelResults({ data }: { data: BacktestRiskModel }) {
	const pct = `${data.confidence * 100}%`;
	const exceptions = data.dates.filter((_, i) => data.exception_days[i]);
	const light = data.traffic_light;
	return (
		<div className="flex flex-col gap-4">
			<GrowthChart
				title={`Hypothetical P&L against the ${pct} VaR announced the evening before`}
				subtitle={`${data.observations} days, in ${data.base_currency}; each day the VaR of the previous day's holdings on the ${data.window} days before it`}
				dates={data.dates}
				series={[
					{ label: "Hypothetical P&L", values: data.pnl },
					{
						label: `−VaR ${pct}`,
						values: data.value_at_risk.map((v) => -v),
						dashed: true,
					},
				]}
				marks={exceptions}
				marksLabel="exception: a loss beyond the VaR"
				yLabel={data.base_currency}
			/>
			<MetricRow className="xl:grid-cols-4">
				<Metric
					label="Exceptions"
					value={`${data.exceptions} of ${data.observations}`}
					footnote={`${data.expected_exceptions.toFixed(1)} expected if the VaR is right.`}
				/>
				<Metric
					label="Kupiec, coverage"
					value={p(data.unconditional_coverage.p_value)}
					unit="p-value"
					footnote={
						data.confidence === 0.99 && data.observations === 250
							? "Is the number of exceptions the expected one? On 250 days at 99 % this test rejects an exact model about one time in ten at the 5 % level, not one in twenty: a low p-value is a warning, not a verdict."
							: "Is the number of exceptions the expected one? With so few expected exceptions the test has little power: a low p-value is a warning, not a verdict."
					}
				/>
				<Metric
					label="Christoffersen, independence"
					value={p(data.independence.p_value)}
					unit="p-value"
					footnote={`Do exceptions cluster? Conditional coverage (both together): ${p(data.conditional_coverage.p_value)}. Too few exceptions leave it untestable (n/a).`}
				/>
				<Metric
					label="Basel traffic light"
					value={light.zone}
					footnote={
						light.basel_plus_factor != null
							? `Plus factor on the capital multiplier: ${light.basel_plus_factor}; probability of so few exceptions or fewer under a right model: ${p(light.cumulative_probability)}.`
							: `Defined for 250 days at 99 %; here a cumulative probability of ${p(light.cumulative_probability)}.`
					}
				/>
			</MetricRow>
			<ul className="list-disc pl-5 text-xs text-ink-secondary">
				<li>
					The model is judged on the hypothetical P&L, not the actual one: the
					day's trades would move the actual P&L without saying anything about
					the VaR.
				</li>
				<li>
					The VaR is the portfolio's historical simulation of the Risk tab,
					computed each evening on the window before it with that day's
					holdings.
				</li>
				{data.skipped > 0 && (
					<li className="text-warning">
						{data.skipped} day(s) left out: no holdings the day before, or too
						few days with a mark behind them.
					</li>
				)}
			</ul>
			<p className="text-2xs text-ink-muted">
				{SOURCES.kupiec} {SOURCES.christoffersen} {SOURCES.basel}
			</p>
		</div>
	);
}
