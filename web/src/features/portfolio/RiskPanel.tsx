import type { RiskAggregate } from "@/shared/portfolio";
import { NumberCell } from "@/shared/ui";
import { AllocationChart } from "@/shared/viz";
import { RateRiskTable } from "./RateRiskTable";
import { RiskTotals } from "./RiskTotals";

const GREEKS = ["delta", "gamma", "vega", "theta", "rho"] as const;
const SYMBOL = { delta: "Δ", gamma: "Γ", vega: "Vega", theta: "Θ", rho: "ρ" };

/**
 * Risk of the portfolio, then per underlying (WP 09 §4). The greeks of the
 * table are grouped PER UNDERLYING — a delta in shares of AAPL does not add
 * to one in barrels of oil. The totals above add money instead (RiskTotals).
 * Delta is in shares of the underlying: a stock contributes its quantity.
 * Rate and FX trades have other risks (RateRiskTable): their equity greeks
 * are not zero, they do not exist.
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
			<RiskTotals totals={risk.totals} base={base} />
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
										{g.greeks ? (
											<NumberCell
												value={g[k]}
												magnitude="greek"
												className="text-xs"
											/>
										) : (
											<span className="text-ink-muted">—</span>
										)}
									</td>
								))}
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<RateRiskTable rows={risk.rates} />
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
