import type { BacktestReport } from "@/shared/api";
import {
	confidence,
	formatMetric,
	interval,
	metricLabel,
	metricOrder,
	provenance,
} from "./format";

export type Column = { label: string; report: BacktestReport };

/**
 * Every metric of every run, never a figure alone: the value, its confidence
 * interval under it, and on hover the convention and the method the engine
 * gives for that interval. A metric the engine could not bracket is listed in
 * `omitted` with its reason, and shown as absent, not as zero. The conventions
 * are also written out under the table, for a reader without a pointer.
 */
export function MetricsTable({ columns }: { columns: Column[] }) {
	const names = metricOrder([
		...new Set(
			columns.flatMap((c) => [
				...Object.keys(c.report.metrics),
				...Object.keys(c.report.omitted),
			]),
		),
	]);
	const reference = columns[0]!.report;
	const level = confidence(Object.values(reference.metrics)[0]);

	return (
		<div className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-3">
			<div className="flex flex-wrap items-baseline justify-between gap-2">
				<h3 className="text-sm font-medium text-ink">Performance</h3>
				<span className="text-2xs text-ink-muted">
					{reference.observations} daily returns, {reference.years.toFixed(1)}{" "}
					years; under each value, its {level} confidence interval
				</span>
			</div>
			<div className="overflow-x-auto">
				<table className="w-full text-right text-xs">
					<thead>
						<tr className="border-b border-hairline text-ink-muted">
							<th className="py-1.5 pr-3 text-left font-medium">Metric</th>
							{columns.map((c) => (
								<th key={c.label} className="px-2 py-1.5 font-medium">
									{c.label}
								</th>
							))}
						</tr>
					</thead>
					<tbody>
						{names.map((name) => (
							<tr key={name} className="border-b border-hairline/60 align-top">
								<th className="py-1.5 pr-3 text-left font-normal text-ink-secondary">
									{metricLabel(name)}
								</th>
								{columns.map((c) => {
									const m = c.report.metrics[name];
									const why = c.report.omitted[name];
									return (
										<td
											key={c.label}
											className="px-2 py-1.5 font-mono tabular-nums"
											title={m ? provenance(m) : why}
										>
											{m ? (
												<>
													<div className="text-ink">
														{formatMetric(name, m.value)}
													</div>
													<div className="text-2xs text-ink-muted">
														{interval(name, m)}
													</div>
												</>
											) : (
												<div className="text-ink-muted">
													{why ? "not available" : "—"}
												</div>
											)}
										</td>
									);
								})}
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<Omitted columns={columns} />
			<details className="text-2xs text-ink-muted">
				<summary className="cursor-pointer select-none">
					Convention and interval of each metric
				</summary>
				<dl className="mt-2 flex flex-col gap-1.5">
					{names
						.filter((n) => reference.metrics[n])
						.map((n) => (
							<div key={n}>
								<dt className="inline font-medium text-ink-secondary">
									{metricLabel(n)}.{" "}
								</dt>
								<dd className="inline">{provenance(reference.metrics[n]!)}</dd>
							</div>
						))}
				</dl>
			</details>
		</div>
	);
}

/** The metrics left out, with the engine's reason for each. */
function Omitted({ columns }: { columns: Column[] }) {
	const reasons = columns.flatMap((c) =>
		Object.entries(c.report.omitted).map(
			([name, why]) => `${metricLabel(name)} (${c.label}): ${why}`,
		),
	);
	if (reasons.length === 0) return null;
	return (
		<ul className="text-2xs text-warning">
			{[...new Set(reasons)].map((r) => (
				<li key={r}>Not available: {r}</li>
			))}
		</ul>
	);
}
