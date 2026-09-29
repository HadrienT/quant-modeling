import type { CreditSpreadsResponse } from "@/shared/api";
import { Badge } from "@/shared/ui";
import { bp, pct } from "./format";

/**
 * Spreads by rating, each turned into a flat hazard rate (5-year CDS
 * bootstrap) and the default probabilities it implies — plus the aggregate
 * indices and Moody's yields for reference.
 */
export function RatingsTable({ data }: { data: CreditSpreadsResponse }) {
	const horizons = data.ratings[0]?.pd_horizons ?? [];
	return (
		<section className="flex flex-col gap-3">
			<div className="flex flex-wrap items-baseline justify-between gap-2">
				<h2 className="text-sm font-semibold text-ink">By rating</h2>
				<span className="text-xs text-ink-muted">
					as of {data.ratings_as_of ?? "—"} · flat hazard, recovery{" "}
					{pct(data.recovery, 0)}
				</span>
			</div>
			<div className="overflow-x-auto rounded-sm border border-hairline">
				<table className="w-full text-sm">
					<thead className="bg-surface-raised text-left text-xs text-ink-secondary">
						<tr>
							<th className="px-3 py-2 font-medium">Rating</th>
							<th className="px-3 py-2 text-right font-medium">OAS</th>
							<th className="px-3 py-2 text-right font-medium">Hazard λ</th>
							{horizons.map((h) => (
								<th key={h} className="px-3 py-2 text-right font-medium">
									PD {h}Y
								</th>
							))}
						</tr>
					</thead>
					<tbody>
						{data.ratings.map((r) => (
							<tr key={r.series_id} className="border-t border-hairline">
								<td className="px-3 py-2 text-ink">
									<span className="flex items-center gap-2">
										{r.label}
										<Badge tone={r.group === "IG" ? "accent" : "warning"}>
											{r.group}
										</Badge>
									</span>
								</td>
								<td className="px-3 py-2 text-right tabular-nums">
									{bp(r.spread, 0)}
								</td>
								<td className="px-3 py-2 text-right tabular-nums">
									{pct(r.hazard)}
								</td>
								{r.default_probabilities.map((p, i) => (
									<td key={i} className="px-3 py-2 text-right tabular-nums">
										{pct(p)}
									</td>
								))}
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<dl className="grid gap-x-6 gap-y-1 text-xs sm:grid-cols-2">
				{[
					...data.aggregates.map((s) => ({ ...s, text: bp(s.value, 0) })),
					...data.reference_yields.map((s) => ({ ...s, text: pct(s.value) })),
				].map((s) => (
					<div key={s.series_id} className="flex justify-between gap-2">
						<dt className="text-ink-secondary">{s.label}</dt>
						<dd className="text-ink tabular-nums">
							{s.text}{" "}
							<span className="text-ink-muted">({s.as_of ?? "—"})</span>
						</dd>
					</div>
				))}
			</dl>
		</section>
	);
}
