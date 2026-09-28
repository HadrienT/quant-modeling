import type { ModelChoice } from "@/shared/api";
import { formatNumber } from "@/shared/format";

/** What each underlying of a multi-asset script was priced with, and the
 * correlation matrix (historical or typed). */
export function UnderlyingsUsed({ choice }: { choice: ModelChoice }) {
	const u = choice.underlyings ?? [];
	const c = choice.correlation ?? [];
	const label = (x: (typeof u)[number]) =>
		x.ticker ?? (x.fx ? `${x.fx}/${x.currency ?? ""}` : undefined);
	const name = (i: number) => (u[i] && label(u[i])) ?? `spot(${i})`;
	const quanto = u.some((x) => x.drift_adjustment);
	return (
		<div className="flex flex-col gap-2 text-2xs">
			<table className="w-full">
				<thead className="text-ink-muted">
					<tr>
						<th className="text-left font-normal">Underlying</th>
						<th className="text-right font-normal">Spot</th>
						<th className="text-right font-normal">Dividend</th>
						<th className="text-right font-normal">Vol</th>
						{quanto && (
							<th
								className="text-right font-normal"
								title="Added to the dividend yield in the simulation"
							>
								Drift adj.
							</th>
						)}
						<th className="pl-3 text-left font-normal">Vol source</th>
					</tr>
				</thead>
				<tbody className="font-mono text-ink">
					{u.map((x, i) => (
						<tr key={i}>
							<td>
								spot({i}){label(x) ? ` = ${label(x)}` : ""}
								{x.currency && x.ticker ? ` (${x.currency})` : ""}
							</td>
							<td className="text-right">{formatNumber(x.spot, "plain")}</td>
							<td className="text-right">{formatNumber(x.dividend, "rate")}</td>
							<td className="text-right">{formatNumber(x.vol, "vol")}</td>
							{quanto && (
								<td className="text-right" title={x.drift_source ?? undefined}>
									{formatNumber(x.drift_adjustment ?? 0, "rate")}
								</td>
							)}
							<td className="pl-3 font-sans text-ink-secondary">
								{x.vol_source}
							</td>
						</tr>
					))}
				</tbody>
			</table>
			{c.length > 1 && (
				<div>
					<p className="text-ink-muted">
						Correlation ({choice.correlation_source ?? "given"})
					</p>
					<table className="font-mono text-ink">
						<tbody>
							{c.map((row, i) => (
								<tr key={i}>
									<td className="pr-3 text-ink-muted">{name(i)}</td>
									{row.map((x, j) => (
										<td key={j} className="pr-3 text-right">
											{formatNumber(x, "plain")}
										</td>
									))}
								</tr>
							))}
						</tbody>
					</table>
				</div>
			)}
		</div>
	);
}
