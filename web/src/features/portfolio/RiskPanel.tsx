import type { RiskAggregate } from "@/shared/portfolio";
import { NumberCell } from "@/shared/ui";
import { AllocationChart } from "@/shared/viz";

const GREEKS = ["delta", "gamma", "vega", "theta", "rho"] as const;
const SYMBOL = { delta: "Δ", gamma: "Γ", vega: "Vega", theta: "Θ", rho: "ρ" };

/**
 * Risk per underlying (WP 09 §4). Greeks are grouped PER UNDERLYING —
 * summing delta across AAPL and oil is a number with no meaning (pitfalls).
 * Delta is in shares of the underlying: a stock contributes its quantity.
 */
export function RiskPanel({
	risk,
	base,
}: {
	risk: RiskAggregate;
	base: string;
}) {
	if (risk.valuedCount === 0) return null;
	const gross = risk.byUnderlying.reduce(
		(s, g) => s + Math.abs(g.marketValue),
		0,
	);

	return (
		<div className="flex flex-col gap-4">
			{risk.valuedCount < risk.openCount && (
				<p className="text-xs text-warning">
					{risk.valuedCount} of {risk.openCount} open positions have a mark: the
					aggregate covers a subset.
				</p>
			)}
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
				<table className="w-full text-xs">
					<thead>
						<tr className="text-2xs text-ink-muted uppercase">
							<th className="p-2 text-left">Underlying</th>
							<th className="p-2 text-right">Market value ({base})</th>
							<th className="p-2 text-right">Unrealised ({base})</th>
							{GREEKS.map((g) => (
								<th key={g} className="p-2 text-right">
									{SYMBOL[g]}
								</th>
							))}
						</tr>
					</thead>
					<tbody>
						{risk.byUnderlying.map((g) => (
							<tr key={g.underlying} className="border-t border-hairline">
								<td className="p-2 text-left text-ink">{g.underlying}</td>
								<td className="p-2 text-right">
									<NumberCell value={g.marketValue} className="text-xs" />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={g.unrealised} signed className="text-xs" />
								</td>
								{GREEKS.map((k) => (
									<td key={k} className="p-2 text-right">
										<NumberCell
											value={g[k]}
											magnitude="greek"
											className="text-xs"
										/>
									</td>
								))}
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<AllocationChart
				title="Gross exposure by underlying"
				rows={risk.byUnderlying.map((g) => ({
					label: g.underlying,
					weight: Math.abs(g.marketValue) / (gross || 1),
				}))}
			/>
		</div>
	);
}
