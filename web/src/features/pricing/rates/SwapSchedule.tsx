import type { SwapPricingResponse } from "@/shared/api";
import { ratePct, signedMoney } from "@/shared/format";

type Swap = SwapPricingResponse["swap"];
type Period = Swap["fixed_periods"][number];

function Leg({ title, periods }: { title: string; periods: Period[] }) {
	return (
		<table className="w-full text-xs">
			<caption className="pb-1 text-left text-ink-muted">{title}</caption>
			<thead>
				<tr className="text-left text-ink-muted">
					<th className="px-2 py-1 font-normal">Period (y)</th>
					<th className="px-2 py-1 text-right font-normal">Rate</th>
					<th className="px-2 py-1 text-right font-normal">Discount</th>
					<th className="px-2 py-1 text-right font-normal">Present value</th>
				</tr>
			</thead>
			<tbody className="font-mono tabular-nums">
				{periods.map((p) => (
					<tr key={p.end} className="border-t border-hairline">
						<td className="px-2 py-1">
							{p.start.toFixed(2)} → {p.end.toFixed(2)}
						</td>
						<td className="px-2 py-1 text-right">{ratePct(p.rate)}</td>
						<td className="px-2 py-1 text-right">{p.discount.toFixed(5)}</td>
						<td className="px-2 py-1 text-right">
							{signedMoney(p.present_value)}
						</td>
					</tr>
				))}
			</tbody>
		</table>
	);
}

/**
 * The swap period by period: the rate of the flow (on the floating leg, the
 * index forward read on the projection curve), the OIS discount factor of
 * its payment date, and its present value signed for the holder.
 */
export function SwapSchedule({ swap }: { swap: Swap }) {
	return (
		<details className="rounded-md border border-hairline bg-surface p-3">
			<summary className="cursor-pointer text-sm text-ink">
				Cash flows, period by period
			</summary>
			<div className="mt-3 grid gap-4 overflow-x-auto xl:grid-cols-2">
				<Leg title="Fixed leg (the fixed rate)" periods={swap.fixed_periods} />
				<Leg
					title="Floating leg (the index forward)"
					periods={swap.floating_periods}
				/>
			</div>
		</details>
	);
}
