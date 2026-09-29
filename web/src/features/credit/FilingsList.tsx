import type { FundamentalsResponse } from "@/shared/api";

/** The company's latest 10-K and 10-Q filings, each linked to its document. */
export function FilingsList({
	filings,
}: {
	filings: FundamentalsResponse["filings"];
}) {
	if (filings.length === 0) return null;
	return (
		<section className="flex flex-col gap-2">
			<h2 className="text-sm font-semibold text-ink">Recent filings</h2>
			<ul className="grid gap-1 text-sm sm:grid-cols-2">
				{filings.map((f) => (
					<li key={f.accession} className="flex items-baseline gap-2">
						<span className="w-14 font-mono text-xs text-ink-secondary">
							{f.form}
						</span>
						{f.url ? (
							<a
								href={f.url}
								target="_blank"
								rel="noreferrer"
								className="text-ink underline decoration-hairline hover:text-accent"
							>
								period {f.report_date ?? "—"}
							</a>
						) : (
							<span>period {f.report_date ?? "—"}</span>
						)}
						<span className="text-xs text-ink-muted">filed {f.filed}</span>
					</li>
				))}
			</ul>
		</section>
	);
}
