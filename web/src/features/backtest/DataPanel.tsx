import type { BacktestStrategy } from "@/shared/api";
import { Badge } from "@/shared/ui";
import { SURVIVORSHIP } from "./texts";

const TITLES: Record<string, string> = {
	strategy: "Strategy",
	execution: "Execution",
	valuation: "Valuation",
	costs: "Costs",
	cash: "Cash",
	corporate_actions: "Dividends and splits",
	turnover: "Turnover",
};

/**
 * What the run stood on: the engine's report on the quality of the data it
 * was given, its conventions word for word (execution timing, valuation,
 * costs, cash, corporate actions), and what identifies the run: the
 * fingerprint of each table read and the build of the engine.
 */
export function DataPanel({ data }: { data: BacktestStrategy }) {
	const names = [
		...Object.keys(TITLES).filter((k) => k in data.conventions),
		...Object.keys(data.conventions).filter((k) => !(k in TITLES)),
	];
	return (
		<div className="flex flex-col gap-4">
			<section className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-3">
				<h3 className="text-sm font-medium text-ink">Quality of the data</h3>
				{data.quality.length === 0 ? (
					<p className="text-xs text-ink-secondary">
						The engine&apos;s controls flagged nothing on these series: no
						extreme return, no stale price, no dividend and split on the same
						day.
					</p>
				) : (
					<ul className="flex flex-col gap-1.5 text-xs text-ink-secondary">
						{data.quality.map((q) => (
							<li key={q.message} className="flex items-start gap-2">
								<Badge tone={q.severity === "error" ? "critical" : "warning"}>
									{q.severity}
								</Badge>
								{q.message}
							</li>
						))}
					</ul>
				)}
				<p className="text-2xs text-ink-muted">{SURVIVORSHIP}</p>
			</section>

			<section className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-3">
				<h3 className="text-sm font-medium text-ink">
					Conventions of the run, as the engine states them
				</h3>
				<dl className="flex flex-col gap-2 text-xs">
					{names.map((k) => (
						<div key={k}>
							<dt className="font-medium text-ink-secondary">
								{TITLES[k] ?? k}
							</dt>
							<dd className="text-ink-secondary">{data.conventions[k]}</dd>
						</div>
					))}
				</dl>
			</section>

			<section className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-3">
				<h3 className="text-sm font-medium text-ink">
					What identifies the run
				</h3>
				<p className="text-xs text-ink-secondary">
					{data.data.days} trading days from {data.data.first} to{" "}
					{data.data.last}. The run is recorded with its request, the
					fingerprints below and the engine&apos;s build: replayed on the same
					three, it gives the same figures to the last bit.
				</p>
				<dl className="grid gap-x-4 gap-y-1 font-mono text-2xs text-ink-muted sm:grid-cols-[auto_1fr]">
					{Object.entries(data.data.fingerprints).map(([name, print]) => (
						<div key={name} className="contents">
							<dt>{name}</dt>
							<dd className="truncate" title={print}>
								sha256:{print}
							</dd>
						</div>
					))}
					<dt>engine</dt>
					<dd className="truncate">
						quantbacktest {data.engine.version} @ {data.engine.git_sha}
					</dd>
					<dt>pricing library</dt>
					<dd className="truncate">
						quantModeling @ {data.engine.quantmodeling_sha}
					</dd>
				</dl>
			</section>
		</div>
	);
}
