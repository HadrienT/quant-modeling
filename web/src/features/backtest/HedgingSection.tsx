import { useState } from "react";
import {
	useBacktestTask,
	useCancelBacktestTask,
	useStartHedging,
} from "@/shared/api";
import { Button } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { HedgingControls } from "./HedgingControls";
import { HedgingResults } from "./HedgingResults";
import { hedgingRequest } from "./hedging";

/**
 * The hedging tab (WP 24, lot B8): an option bought and delta-hedged in a
 * simulated world, run as a task on the server. The page follows its
 * progress, can cancel it, and shows the result when it is done; a Heston
 * world takes a minute or more, a Black-Scholes one about a second.
 */
export function HedgingSection() {
	const start = useStartHedging();
	const cancel = useCancelBacktestTask();
	const [taskId, setTaskId] = useState<string | null>(null);
	const task = useBacktestTask(taskId);
	const status = task.data?.status;
	const busy = start.isPending || status === "queued" || status === "running";

	return (
		<div className="flex flex-col gap-4">
			<p className="text-sm text-ink-secondary">
				A bought option delta-hedged until maturity in a market whose model is
				known: what is left of the P&L under each hedge, on the same paths. The
				world is simulated, so this measures the hedges, not a history.
			</p>
			<HedgingControls
				running={busy}
				onRun={(form) =>
					start.mutate(hedgingRequest(form), {
						onSuccess: (t) => setTaskId(t.id),
					})
				}
			/>
			{start.error && <ErrorState error={start.error} />}
			{task.error && <ErrorState error={task.error} />}
			{task.data && busy && (
				<div className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-3">
					<div className="flex items-center justify-between gap-3 text-sm">
						<span className="text-ink-secondary">
							{status === "queued"
								? `Queued: ${task.data.ahead} backtest(s) ahead`
								: task.data.step || "Running"}
						</span>
						<Button
							variant="secondary"
							size="sm"
							disabled={cancel.isPending}
							onClick={() => taskId && cancel.mutate(taskId)}
						>
							Cancel
						</Button>
					</div>
					<div
						className="h-1.5 w-full overflow-hidden rounded bg-hairline"
						role="progressbar"
						aria-valuenow={Math.round(task.data.progress * 100)}
						aria-valuemin={0}
						aria-valuemax={100}
					>
						<div
							className="h-full bg-accent transition-[width]"
							style={{ width: `${task.data.progress * 100}%` }}
						/>
					</div>
				</div>
			)}
			{status === "failed" && (
				<ErrorState error={new Error(task.data?.error ?? "The run failed")} />
			)}
			{status === "cancelled" && (
				<p className="text-sm text-ink-muted">Cancelled.</p>
			)}
			{status === "done" && task.data?.hedging && (
				<HedgingResults data={task.data.hedging} />
			)}
		</div>
	);
}
