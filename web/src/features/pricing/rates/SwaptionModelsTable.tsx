import type { SwaptionPricingResponse } from "@/shared/api";
import { rateBp, signedMoney } from "@/shared/format";

type Swaption = SwaptionPricingResponse["swaption"];

/**
 * The European swaption under each model that has its inputs, with the
 * normal vol each price implies: the models agree on the forward and the
 * annuity, and differ by what they assume about the swap rate. The row of
 * the model the value is under is marked.
 */
export function SwaptionModelsTable({ swaption }: { swaption: Swaption }) {
	const priced = swaption.exercise === "european" ? swaption.model : null;
	return (
		<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
			<table className="w-full text-sm">
				<caption className="p-2 text-left text-xs text-ink-muted">
					European swaption, model by model
				</caption>
				<thead>
					<tr className="text-left text-xs text-ink-muted">
						<th className="px-2 py-1 font-normal">Model</th>
						<th className="px-2 py-1 text-right font-normal">Value</th>
						<th className="px-2 py-1 text-right font-normal">
							Implied normal vol
						</th>
						<th className="px-2 py-1 font-normal">Parameters</th>
					</tr>
				</thead>
				<tbody>
					{swaption.prices.map((p) => (
						<tr
							key={p.key}
							aria-current={p.key === priced ? "true" : undefined}
							className={
								"border-t border-hairline" +
								(p.key === priced ? " bg-surface-raised" : "")
							}
						>
							<td className="px-2 py-1">
								{p.model}
								{p.key === priced && (
									<span className="ml-1 text-2xs text-accent">· priced</span>
								)}
							</td>
							<td className="px-2 py-1 text-right font-mono tabular-nums">
								{signedMoney(p.price)}
							</td>
							<td className="px-2 py-1 text-right font-mono tabular-nums">
								{rateBp(p.implied_normal_vol)}
							</td>
							<td className="px-2 py-1 text-xs text-ink-secondary">
								{p.detail}
							</td>
						</tr>
					))}
				</tbody>
			</table>
		</div>
	);
}
