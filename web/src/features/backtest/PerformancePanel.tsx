import type { BacktestStrategy } from "@/shared/api";
import { GrowthChart, type GrowthSeries, WeightsChart } from "@/shared/viz";
import { MetricsTable } from "./MetricsTable";
import { INITIAL_CAPITAL } from "./state";

/**
 * The benchmark is a series of total returns, not a book: to be drawn next
 * to the books it is rebased to their initial capital. A change of
 * representation for the eye; its metrics come from the engine.
 */
function rebased(returns: (number | null)[]): number[] {
	const out = [INITIAL_CAPITAL];
	for (const r of returns) out.push(out[out.length - 1]! * (1 + (r ?? 0)));
	return out;
}

/**
 * The strategy and its comparison points, always drawn together: the same
 * assets held in equal weights, the same assets bought once and left alone,
 * and the benchmark with its dividends. Net of costs next to gross. Under
 * the curves, a mark for every day a decision was taken, and below, what was
 * held between two decisions.
 */
export function PerformancePanel({ data }: { data: BacktestStrategy }) {
	const benchmark = `${data.benchmark_ticker}, dividends reinvested`;
	const series: GrowthSeries[] = [
		{ label: "Strategy, net of costs", values: data.strategy.value ?? [] },
		...(data.gross
			? [
					{
						label: "Strategy, before costs",
						values: data.gross.value ?? [],
						dashed: true,
					},
				]
			: []),
		{
			label: "Equal weight",
			values: data.equal_weight.value ?? [],
			dashed: true,
		},
		{
			label: "Buy and hold",
			values: data.buy_and_hold.value ?? [],
			dashed: true,
		},
		{
			label: benchmark,
			values: rebased(data.benchmark.returns ?? []),
			dashed: true,
		},
	];
	const columns = [
		{ label: "Net", report: data.strategy.report },
		...(data.gross ? [{ label: "Gross", report: data.gross.report }] : []),
		{ label: "Equal weight", report: data.equal_weight.report },
		{ label: "Buy and hold", report: data.buy_and_hold.report },
		{ label: data.benchmark_ticker, report: data.benchmark.report },
	];

	return (
		<div className="flex flex-col gap-4">
			<GrowthChart
				title="Value of 1,000,000 invested on the first day"
				subtitle={`Books in shares and cash, on prices as quoted, dividends and splits applied on their date. ${benchmark}, is rebased to the same capital.`}
				dates={data.dates}
				series={series}
				marks={data.allocations.dates}
				marksLabel="rebalancing decision"
				yLabel="USD"
			/>
			<WeightsChart
				title="What the strategy held"
				subtitle="Target weights decided on each rebalancing date, held until the next."
				dates={data.allocations.dates}
				end={data.dates[data.dates.length - 1]!}
				labels={data.tickers}
				weights={data.allocations.weights}
			/>
			<MetricsTable columns={columns} />
		</div>
	);
}
