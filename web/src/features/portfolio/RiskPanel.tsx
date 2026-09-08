import type { RiskAggregate } from "@/shared/portfolio";
import {
	DeltaBadge,
	Freshness,
	Metric,
	MetricRow,
	NumberCell,
} from "@/shared/ui";
import { AllocationChart } from "@/shared/viz";

/**
 * Aggregated risk (WP 09 §4). Greeks are grouped PER UNDERLYING — summing delta
 * across AAPL and oil is a number with no meaning (pitfalls). positions_priced /
 * total is shown so a partial aggregate says so.
 */
export function RiskPanel({ risk }: { risk: RiskAggregate }) {
	if (risk.pricedCount === 0) {
		return (
			<p className="text-sm text-ink-muted">
				No positions priced yet — run "Price all positions".
			</p>
		);
	}

	return (
		<div className="flex flex-col gap-4">
			<MetricRow>
				<Metric
					label="Total NPV"
					value={<NumberCell value={risk.totalNpv} magnitude="price" />}
				/>
				<Metric
					label="Total P&L"
					value={<DeltaBadge value={risk.totalPnl} magnitude="price" />}
				/>
				<Metric
					label="Positions priced"
					value={`${risk.pricedCount} / ${risk.totalCount}`}
					footnote={
						risk.pricedCount < risk.totalCount
							? "aggregate covers a subset"
							: undefined
					}
				/>
				<Metric label="Underlyings" value={String(risk.byUnderlying.length)} />
				<Metric
					label="Oldest price"
					value={
						risk.oldestPricedAt ? (
							<Freshness at={risk.oldestPricedAt} label="" />
						) : (
							"—"
						)
					}
				/>
			</MetricRow>

			<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
				<table className="w-full text-xs">
					<thead>
						<tr className="text-2xs text-ink-muted uppercase">
							{[
								"Underlying",
								"NPV",
								"P&L",
								"Δ",
								"Γ",
								"Vega",
								"Θ",
								"ρ",
								"MC ±",
							].map((h) => (
								<th key={h} className="p-2 text-right first:text-left">
									{h}
								</th>
							))}
						</tr>
					</thead>
					<tbody className="font-mono tabular-nums">
						{risk.byUnderlying.map((g) => (
							<tr key={g.underlying} className="border-t border-hairline">
								<td className="p-2 text-left text-ink">{g.underlying}</td>
								<td className="p-2 text-right">
									<NumberCell value={g.npv} magnitude="price" />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={g.pnl} magnitude="price" signed />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={g.delta} magnitude="greek" />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={g.gamma} magnitude="greek" />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={g.vega} magnitude="greek" />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={g.theta} magnitude="greek" />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={g.rho} magnitude="greek" />
								</td>
								<td className="p-2 text-right text-ink-muted">
									{g.mcStdError > 0 ? g.mcStdError.toPrecision(2) : "—"}
								</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>

			<AllocationChart
				title="NPV concentration by underlying"
				rows={risk.byUnderlying.map((g) => ({
					label: g.underlying,
					weight: Math.abs(g.npv) / (Math.abs(risk.totalNpv) || 1),
				}))}
			/>
		</div>
	);
}
