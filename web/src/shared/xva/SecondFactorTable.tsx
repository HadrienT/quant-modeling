import type { XvaQuoteRisk } from "@/shared/api";
import type { RiskKey } from "./SensitivityBars";
import { signed } from "./format";

/**
 * The sensitivities to the second factor of a netting set — an exchange rate
 * or a share: its price, its market volatility, its curve, the correlations.
 * They are not all rates, so each row says the move it is for: 1 % of a
 * price, a volatility point, 0.01 of a correlation, a basis point.
 */
export function SecondFactorTable({
	risks,
	adjustment,
}: {
	risks: XvaQuoteRisk[];
	adjustment: RiskKey;
}) {
	if (risks.length === 0) return null;
	return (
		<section className="flex flex-col gap-2">
			<h4 className="text-xs font-semibold tracking-wide text-ink-secondary uppercase">
				Exchange rate or share
			</h4>
			<div className="overflow-x-auto">
				<table className="w-full text-sm">
					<caption className="sr-only">
						Change of the adjustment for a move of each quantity of the second
						factor
					</caption>
					<thead className="text-left text-xs text-ink-muted">
						<tr>
							<th className="px-2 py-1 font-normal">Quantity</th>
							<th className="px-2 py-1 text-right font-normal">Today</th>
							<th className="px-2 py-1 text-right font-normal">For</th>
							<th className="px-2 py-1 text-right font-normal">
								Change of the adjustment
							</th>
						</tr>
					</thead>
					<tbody>
						{risks.map((r) => {
							const move = r.move ?? 1e-4;
							const v = r[adjustment];
							return (
								<tr key={r.label} className="border-t border-hairline">
									<th
										scope="row"
										className="px-2 py-1 text-left text-xs font-normal text-ink-secondary"
									>
										{r.label}
									</th>
									<td className="px-2 py-1 text-right text-xs tabular-nums">
										{Math.abs(r.level) < 10
											? r.level.toFixed(4)
											: r.level.toLocaleString("en-US", {
													maximumFractionDigits: 2,
												})}
									</td>
									<td className="px-2 py-1 text-right text-xs text-ink-secondary">
										{r.move_label ?? "+1 bp"}
									</td>
									<td className="px-2 py-1 text-right text-xs tabular-nums">
										{signed(v.value * move)}{" "}
										<span className="text-ink-muted">
											± {Math.abs(v.error * move).toFixed(1)}
										</span>
									</td>
								</tr>
							);
						})}
					</tbody>
				</table>
			</div>
		</section>
	);
}
