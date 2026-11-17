import { useState } from "react";
import {
	type Portfolio,
	useCancelPortfolioVar,
	usePortfolioVarTask,
	useStartPortfolioVar,
} from "@/shared/api";
import { Button } from "@/shared/ui";
import { Metric, MetricRow } from "@/shared/ui/density";
import { ErrorState } from "@/shared/ui/states";
import { DistributionChart } from "@/shared/viz";

const money = (v: number) =>
	new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 }).format(v);

/**
 * Historical value at risk of the ledger (issue 292): today's holdings
 * applied to each day's change of every instrument over a year of marks, each
 * derivative fully revalued with that day's market. The first run revalues
 * the derivatives on every day of the window (minutes), so it is a task the
 * page follows; the marks are kept, and the next run is quick.
 */
export function VarPanel({ pf }: { pf: Portfolio }) {
	const start = useStartPortfolioVar();
	const cancel = useCancelPortfolioVar();
	const [taskId, setTaskId] = useState<string | null>(null);
	const task = usePortfolioVarTask(taskId);
	const status = task.data?.status;
	const busy = start.isPending || status === "queued" || status === "running";
	const v = status === "done" ? task.data?.var : null;

	return (
		<section className="flex flex-col gap-2">
			<div className="flex flex-wrap items-center justify-between gap-2">
				<h3 className="text-sm font-medium text-ink">Value at risk, one day</h3>
				<div className="flex items-center gap-2">
					{busy && taskId && (
						<Button
							size="sm"
							variant="secondary"
							onClick={() => cancel.mutate(taskId)}
						>
							Cancel
						</Button>
					)}
					<Button
						size="sm"
						disabled={busy}
						onClick={() =>
							start.mutate(pf, { onSuccess: (t) => setTaskId(t.id) })
						}
					>
						{v ? "Run again" : "Compute"}
					</Button>
				</div>
			</div>
			{start.error && <ErrorState error={start.error} />}
			{task.error && <ErrorState error={task.error} />}
			{status === "failed" && (
				<ErrorState error={new Error(task.data?.error ?? "The run failed")} />
			)}
			{busy && task.data && (
				<div className="flex flex-col gap-1 text-xs text-ink-secondary">
					<span>
						{status === "queued"
							? `Queued: ${task.data.ahead} run(s) ahead`
							: task.data.step || "Running"}
					</span>
					<div className="h-1.5 w-full overflow-hidden rounded bg-hairline">
						<div
							className="h-full bg-accent"
							style={{ width: `${task.data.progress * 100}%` }}
						/>
					</div>
				</div>
			)}
			{!v && !busy && (
				<p className="text-xs text-ink-muted">
					Today's holdings on each day of the last 250 business days: the first
					run revalues every derivative on each of them.
				</p>
			)}
			{v && (
				<>
					<MetricRow className="xl:grid-cols-4">
						{v.levels.flatMap((L) => [
							<Metric
								key={`var${L.confidence}`}
								label={`VaR ${L.confidence * 100}%`}
								value={money(L.var.value)}
								unit={v.base_currency}
								footnote={`95% interval [${money(L.var.lower)}, ${money(L.var.upper)}]: ${L.var.method}.`}
							/>,
							<Metric
								key={`es${L.confidence}`}
								label={`Expected shortfall ${L.confidence * 100}%`}
								value={money(L.expected_shortfall.value)}
								unit={v.base_currency}
								footnote={`95% interval [${money(L.expected_shortfall.lower)}, ${money(L.expected_shortfall.upper)}]: ${L.expected_shortfall.method}.`}
							/>,
						])}
					</MetricRow>
					<DistributionChart
						title="Today's holdings on each day of the window"
						subtitle={`${v.scenarios.length} days, ${v.window_start} to ${v.window_end}; lines at the 99% VaR and expected shortfall`}
						samples={v.scenarios}
						varLevel={-v.levels[0]!.var.value}
						expectedShortfall={-v.levels[0]!.expected_shortfall.value}
						xLabel={`P&L (${v.base_currency})`}
						xFormat={(x) => money(x)}
					/>
					<ul className="flex flex-col gap-0.5 text-2xs text-ink-muted">
						<li>
							A share's change is its return times today's value; a derivative's
							or a rate trade's, the change of its value per unit, fully
							revalued each day. Positions are today's: trades of the window are
							not replayed.
						</li>
						{v.days_dropped > 0 && (
							<li className="text-warning">
								{v.days_dropped} day(s) without a mark for every position are
								left out.
							</li>
						)}
						{v.left_out.length > 0 && (
							<li className="text-warning">
								Without a mark today, not in the VaR: {v.left_out.join(", ")}.
							</li>
						)}
					</ul>
				</>
			)}
		</section>
	);
}
