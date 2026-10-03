import type { XvaResponse } from "@/shared/api";
import { signed } from "./format";

/**
 * What each trade contributes to the CVA of the set: alone, as the last one
 * added, and as its share of the total — three different questions.
 */
export function TradesTable({ data }: { data: XvaResponse }) {
	return (
		<section className="flex flex-col gap-2">
			<h3 className="text-sm font-semibold text-ink">
				The share of each trade
			</h3>
			<div className="overflow-x-auto rounded-sm border border-hairline">
				<table className="w-full text-sm">
					<thead className="bg-surface-raised text-left text-xs text-ink-secondary">
						<tr>
							<th className="px-3 py-2 font-medium">Trade</th>
							<th className="px-3 py-2 text-right font-medium">Value today</th>
							<th
								className="px-3 py-2 text-right font-medium"
								title="The CVA of the trade as if it were alone"
							>
								Stand-alone CVA
							</th>
							<th
								className="px-3 py-2 text-right font-medium"
								title="The CVA of the set minus its CVA without the trade: the price of adding it"
							>
								Incremental CVA
							</th>
							<th
								className="px-3 py-2 text-right font-medium"
								title="Euler allocation: the shares add up to the total. None under a CSA."
							>
								Marginal CVA
							</th>
						</tr>
					</thead>
					<tbody>
						{data.trades.map((t, i) => (
							<tr key={i} className="border-t border-hairline">
								<td className="px-3 py-2 text-ink">{t.description}</td>
								<td className="px-3 py-2 text-right tabular-nums">
									{signed(t.value_today)}
								</td>
								<td className="px-3 py-2 text-right tabular-nums">
									{signed(t.standalone_cva)}
								</td>
								<td className="px-3 py-2 text-right tabular-nums">
									{signed(t.incremental_cva)}
								</td>
								<td className="px-3 py-2 text-right tabular-nums">
									{signed(t.marginal_cva)}
								</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<p className="text-xs text-ink-muted">
				A positive incremental CVA means the trade lowers the counterparty risk
				of what is already there: it offsets other trades of the set.
			</p>
		</section>
	);
}
