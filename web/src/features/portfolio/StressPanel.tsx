import { type Portfolio, usePortfolioStress } from "@/shared/api";
import { NumberCell } from "@/shared/ui";
import { ErrorState, TableSkeleton } from "@/shared/ui/states";

/**
 * Stress tests of the ledger (issue 292): every position revalued by its desk
 * model under a moved market of today, against its mark. One row per
 * scenario, one column per underlying, the total last. A position the
 * scenario cannot reach (a volatility surface that is not shifted here) is
 * listed under the table, never counted as unmoved.
 */
export function StressPanel({ pf }: { pf: Portfolio }) {
	const q = usePortfolioStress(pf);
	if (q.error)
		return <ErrorState error={q.error} onRetry={() => q.refetch()} />;
	if (!q.data) return <TableSkeleton rows={9} />;
	const { scenarios, base_currency: base, warnings } = q.data;
	const underlyings = [
		...new Set(scenarios.flatMap((s) => Object.keys(s.by_underlying))),
	];
	const notes = scenarios.flatMap((s) =>
		s.rows
			.filter((r) => r.pnl == null)
			.map((r) => `${s.name}: ${r.label} not revalued (${r.note}).`),
	);

	return (
		<section className="flex flex-col gap-2">
			<div className="flex flex-wrap items-baseline justify-between gap-2">
				<h3 className="text-sm font-medium text-ink">Stress tests</h3>
				<span className="text-2xs text-ink-muted">
					P&L in {base} if the market of {q.data.as_of} moved so; every position
					fully revalued
				</span>
			</div>
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
				<table className="w-full text-xs">
					<thead>
						<tr className="text-2xs text-ink-muted uppercase">
							<th className="p-2 text-left">Scenario</th>
							{underlyings.map((u) => (
								<th key={u} className="p-2 text-right">
									{u}
								</th>
							))}
							<th className="p-2 text-right">Total</th>
						</tr>
					</thead>
					<tbody>
						{scenarios.map((s) => (
							<tr key={s.name} className="border-t border-hairline">
								<td className="p-2 text-left text-ink" title={s.description}>
									{s.name}
									{s.not_revalued.length > 0 && (
										<span className="text-warning"> *</span>
									)}
								</td>
								{underlyings.map((u) => (
									<td key={u} className="p-2 text-right">
										{u in s.by_underlying ? (
											<NumberCell
												value={s.by_underlying[u]!}
												className="text-xs"
												signed
											/>
										) : (
											<span className="text-ink-muted">—</span>
										)}
									</td>
								))}
								<td className="p-2 text-right font-medium">
									<NumberCell value={s.pnl} className="text-xs" signed />
								</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<ul className="flex flex-col gap-0.5 text-2xs text-ink-muted">
				<li>
					Spot shocks hold the smile in strike (sticky strike); the volatility
					shift moves the vol each model runs at; the rate shift moves every
					zero rate of every curve. Hover a scenario for its definition.
				</li>
				{[...warnings, ...notes].map((n) => (
					<li key={n} className="text-warning">
						* {n}
					</li>
				))}
			</ul>
		</section>
	);
}
