import { useCreditSpreads, useStructural } from "@/shared/api";
import { Methodology, Metric, MetricRow, WarningList } from "@/shared/ui";
import { ErrorState, MetricRowSkeleton } from "@/shared/ui/states";
import { RatesCurveChart } from "@/shared/viz";
import { bp, count, pct, tenorLabel, usd } from "./format";

/**
 * Merton's structural model for the company: the inputs read from the store
 * (price, share count, realised vol, debt), the calibrated asset value and
 * volatility, and the model's spread and default probability by maturity —
 * drawn against the market's investment-grade spreads, because the gap
 * between the two is the model's known limitation.
 */
export function StructuralPanel({ ticker }: { ticker: string }) {
	const model = useStructural(ticker);
	const market = useCreditSpreads(0.4);
	const r = model.data;

	if (model.error)
		return <ErrorState error={model.error} onRetry={() => model.refetch()} />;
	if (!r) return <MetricRowSkeleton />;

	const i = r.inputs;
	const m = r.maturities ?? [];
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">
				Structural credit model (Merton 1974)
			</h2>
			<WarningList warnings={r.warnings} />
			{r.unavailable ? (
				<p className="rounded-sm border border-hairline bg-surface p-3 text-sm text-ink-secondary">
					{r.unavailable}
				</p>
			) : (
				i && (
					<>
						<MetricRow>
							<Metric
								label="Equity value"
								value={usd(i.equity_value)}
								footnote={`$${i.price.toFixed(2)} × ${count(i.shares_outstanding)} shares (${i.price_date})`}
							/>
							<Metric
								label="Equity vol"
								value={pct(i.equity_vol, 1)}
								footnote={`realised, ${i.vol_observations} daily returns`}
							/>
							<Metric
								label="Default point"
								value={usd(i.default_point)}
								footnote={`ST + ½ LT debt, ${i.balance_sheet_date}`}
							/>
							<Metric
								label="Asset value · vol"
								value={usd(r.asset_value)}
								footnote={`σ_V ${pct(r.asset_vol, 1)} · r ${pct(i.rate)}`}
							/>
							<Metric
								label="Leverage D / V"
								value={pct(r.leverage, 1)}
								footnote={`distance to default 1Y: ${(r.distances_to_default?.[m.indexOf(1)] ?? NaN).toFixed(2)} σ`}
							/>
						</MetricRow>
						<div className="grid gap-4 lg:grid-cols-[3fr_2fr]">
							<RatesCurveChart
								title="Credit spread by maturity"
								height={260}
								series={[
									{
										label: `Merton spread — ${r.ticker}`,
										markers: true,
										points: m.map((t, k) => ({ x: t, y: r.spreads![k]! })),
									},
									{
										label: "Market: US IG OAS by bucket",
										line: false,
										points:
											market.data?.term_structure.map((q) => ({
												x: q.tenor,
												y: q.spread,
											})) ?? [],
									},
								]}
							/>
							<table className="h-fit w-full rounded-sm border border-hairline text-sm">
								<thead className="bg-surface-raised text-xs text-ink-secondary">
									<tr>
										<th className="px-2 py-1.5 text-left font-medium">T</th>
										<th className="px-2 py-1.5 text-right font-medium">
											Spread
										</th>
										<th className="px-2 py-1.5 text-right font-medium">
											PD (Q)
										</th>
										<th className="px-2 py-1.5 text-right font-medium">DD</th>
										<th className="px-2 py-1.5 text-right font-medium">E[R]</th>
									</tr>
								</thead>
								<tbody>
									{m.map((t, k) => (
										<tr
											key={t}
											className="border-t border-hairline tabular-nums"
										>
											<td className="px-2 py-1">{tenorLabel(t)}</td>
											<td className="px-2 py-1 text-right">
												{bp(r.spreads![k])}
											</td>
											<td className="px-2 py-1 text-right">
												{pct(r.default_probabilities![k], 3)}
											</td>
											<td className="px-2 py-1 text-right">
												{r.distances_to_default![k]!.toFixed(2)}
											</td>
											<td className="px-2 py-1 text-right">
												{pct(r.expected_recoveries![k], 0)}
											</td>
										</tr>
									))}
								</tbody>
							</table>
						</div>
					</>
				)
			)}
			<Methodology
				id="structural-methodology"
				title="Structural model — methodology"
				sections={r.methodology}
			/>
		</section>
	);
}
