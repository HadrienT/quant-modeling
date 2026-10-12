import type { XvaSensitivities } from "@/shared/api";
import { amount, pct, tenorLabel } from "./format";

/**
 * SA-CVA: the capital for CVA risk computed from sensitivities (Basel
 * framework, MAR50) — each sensitivity, its supervisory risk weight, and the
 * capital of each of the three risk classes.
 */
export function SaCvaTable({ sa }: { sa: XvaSensitivities["sa_cva"] }) {
	const rows: { name: string; detail: string; capital: number }[] = [
		{
			name: "Interest-rate delta",
			detail: sa.interest_rate_tenors
				.map(
					(t, i) =>
						`${tenorLabel(t)} ${pct(sa.interest_rate_risk_weights[i], 2)}`,
				)
				.join(" · "),
			capital: sa.capital_interest_rate_delta,
		},
		{
			name: "Interest-rate vega",
			detail: "All volatilities moved by the same share · 100 %",
			capital: sa.capital_interest_rate_vega,
		},
		{
			name: "Counterparty credit spread delta",
			detail: `${sa.credit_spread_tenors.map(tenorLabel).join(" · ")} · ${pct(
				sa.credit_spread_risk_weight,
				1,
			)} (${sa.sector.replaceAll("_", " ")}, ${
				sa.investment_grade ? "investment grade" : "high yield"
			})`,
			capital: sa.capital_credit_spread_delta,
		},
	];
	return (
		<section className="flex flex-col gap-2">
			<h4 className="text-xs font-semibold tracking-wide text-ink-secondary uppercase">
				SA-CVA capital from these sensitivities
			</h4>
			<div className="overflow-x-auto rounded-sm border border-hairline">
				<table className="w-full text-sm">
					<thead className="bg-surface-raised text-left text-xs text-ink-secondary">
						<tr>
							<th className="px-3 py-2 font-medium">Risk class</th>
							<th className="px-3 py-2 font-medium">Tenors and risk weights</th>
							<th className="px-3 py-2 text-right font-medium">Capital</th>
						</tr>
					</thead>
					<tbody>
						{rows.map((r) => (
							<tr key={r.name} className="border-t border-hairline">
								<td className="px-3 py-2 text-ink">{r.name}</td>
								<td className="px-3 py-2 text-xs text-ink-secondary">
									{r.detail}
								</td>
								<td className="px-3 py-2 text-right tabular-nums">
									{amount(r.capital)}
								</td>
							</tr>
						))}
						<tr className="border-t border-hairline bg-surface-raised font-medium">
							<td className="px-3 py-2 text-ink" colSpan={2}>
								SA-CVA capital
							</td>
							<td className="px-3 py-2 text-right tabular-nums">
								{amount(sa.capital)}
							</td>
						</tr>
					</tbody>
				</table>
			</div>
			<p className="text-xs text-ink-muted">
				Computed on the unilateral CVA, as the rule asks: the bank&apos;s own
				default is left out. The classes add up; within a class the weighted
				sensitivities are aggregated with supervisory correlations.
			</p>
		</section>
	);
}
