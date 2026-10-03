import type { XvaQuoteRisk } from "@/shared/api";
import { perBp } from "./format";

export type RiskKey = "cva" | "dva" | "fca" | "fba";

/**
 * Sensitivities to a family of quotes, one bar each, per basis point of the
 * quote: to the left what makes the adjustment more negative (a cost that
 * grows), to the right what makes it more positive. Each bar carries its
 * Monte-Carlo error.
 */
export function SensitivityBars({
	title,
	risks,
	adjustment,
	label,
}: {
	title: string;
	risks: XvaQuoteRisk[];
	adjustment: RiskKey;
	label: (r: XvaQuoteRisk) => string;
}) {
	const largest = Math.max(
		...risks.map((r) => Math.abs(r[adjustment].value)),
		1e-12,
	);
	return (
		<section className="flex flex-col gap-2">
			<h4 className="text-xs font-semibold tracking-wide text-ink-secondary uppercase">
				{title}
			</h4>
			<table className="w-full text-sm">
				<caption className="sr-only">
					{title}: change of the adjustment for one basis point
				</caption>
				<tbody>
					{risks.map((r) => {
						const v = r[adjustment];
						const share = (Math.abs(v.value) / largest) * 50;
						return (
							<tr key={r.label} className="border-t border-hairline">
								<th
									scope="row"
									className="w-32 px-2 py-1 text-left text-xs font-normal text-ink-secondary"
								>
									{label(r)}
								</th>
								<td className="px-2 py-1">
									<div
										className="relative h-3 w-full rounded-xs bg-surface-raised"
										aria-hidden
									>
										<div className="absolute top-0 bottom-0 left-1/2 w-px bg-hairline" />
										<div
											className={
												"absolute top-0 bottom-0 " +
												(v.value < 0 ? "bg-warning" : "bg-accent")
											}
											style={
												v.value < 0
													? { right: "50%", width: `${share}%` }
													: { left: "50%", width: `${share}%` }
											}
										/>
									</div>
								</td>
								<td className="w-36 px-2 py-1 text-right text-xs tabular-nums">
									{perBp(v.value)}{" "}
									<span className="text-ink-muted">
										± {(v.error * 1e-4).toFixed(1)}
									</span>
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>
		</section>
	);
}
