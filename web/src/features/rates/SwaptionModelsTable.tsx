import type { RatesAnalysisResponse } from "@/shared/api";
import { bp, money } from "./format";

/** The swaption's value under each model and the normal vol it implies. */
export function SwaptionModelsTable({
	prices,
}: {
	prices: RatesAnalysisResponse["swaption"]["prices"];
}) {
	return (
		<div className="overflow-x-auto">
			<table className="w-full text-sm">
				<caption className="sr-only">Swaption value by model</caption>
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
					{prices.map((p) => (
						<tr key={p.model} className="border-t border-hairline">
							<td className="px-2 py-1">{p.model}</td>
							<td className="px-2 py-1 text-right font-mono tabular-nums">
								{money(p.price)}
							</td>
							<td className="px-2 py-1 text-right font-mono tabular-nums">
								{bp(p.implied_normal_vol)}
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
