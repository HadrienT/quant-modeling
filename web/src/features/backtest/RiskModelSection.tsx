import { useQuery } from "@tanstack/react-query";
import { useState } from "react";
import {
	type Portfolio,
	useBacktestTask,
	useCancelBacktestTask,
	usePortfolioDemos,
	useStartRiskModel,
} from "@/shared/api";
import { localRepository, serverRepository } from "@/shared/portfolio";
import { useMe } from "@/shared/session";
import { Button, Segmented } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { RiskModelResults } from "./RiskModelResults";

/**
 * The risk-model tab (WP 24, lot B8): a portfolio's value at risk backtested
 * on its hypothetical P&L over the last year. Any portfolio: the demos, and
 * the visitor's own (this browser's, or the server's when signed in). A task:
 * the first run revalues the portfolio's derivatives on every day of two
 * years of windows.
 */
export function RiskModelSection() {
	const me = useMe();
	const repo = me.data?.username ? serverRepository : localRepository;
	const demos = usePortfolioDemos();
	const mine = useQuery({
		queryKey: ["backtest", "risk-model", "portfolios", repo.kind],
		queryFn: () => repo.list(),
	});
	const [choice, setChoice] = useState("");
	const [confidence, setConfidence] = useState<0.99 | 0.975>(0.99);
	const start = useStartRiskModel();
	const cancel = useCancelBacktestTask();
	const [taskId, setTaskId] = useState<string | null>(null);
	const task = useBacktestTask(taskId);
	const status = task.data?.status;
	const busy = start.isPending || status === "queued" || status === "running";

	const options = [
		...(mine.data ?? []).map((p) => ({ key: `mine:${p.id}`, label: p.name })),
		...(demos.data ?? []).map((d) => ({
			key: `demo:${d.portfolio.id}`,
			label: `${d.portfolio.name} (demo)`,
		})),
	];
	const selected = choice || options[0]?.key || "";

	async function run() {
		const [kind, id] = [selected.slice(0, 4), selected.slice(5)];
		const pf: Portfolio | null | undefined =
			kind === "demo"
				? demos.data?.find((d) => d.portfolio.id === id)?.portfolio
				: await repo.get(id);
		if (!pf) return;
		start.mutate(
			{ portfolio: pf as never, end: null, days: 250, window: 250, confidence },
			{ onSuccess: (t) => setTaskId(t.id) },
		);
	}

	return (
		<div className="flex flex-col gap-4">
			<p className="text-sm text-ink-secondary">
				Was a portfolio's value at risk right? Each day of the last year, the
				VaR announced the evening before against what the day's market did to
				the positions held.
			</p>
			<div className="flex flex-wrap items-end gap-3 rounded-md border border-hairline bg-surface p-4">
				<label className="flex flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">Portfolio</span>
					<select
						className="min-w-64 rounded border border-hairline bg-surface px-2 py-1"
						value={selected}
						onChange={(e) => setChoice(e.target.value)}
					>
						{options.map((o) => (
							<option key={o.key} value={o.key}>
								{o.label}
							</option>
						))}
					</select>
				</label>
				<Segmented
					label="VaR confidence"
					options={[
						{ value: "0.99", label: "VaR 99%" },
						{ value: "0.975", label: "VaR 97.5%" },
					]}
					value={String(confidence)}
					onChange={(v) => setConfidence(Number(v) as 0.99 | 0.975)}
				/>
				<Button onClick={run} disabled={busy || !selected}>
					{busy ? "Running…" : "Backtest the VaR"}
				</Button>
				{busy && taskId && (
					<Button variant="secondary" onClick={() => cancel.mutate(taskId)}>
						Cancel
					</Button>
				)}
			</div>
			{start.error && <ErrorState error={start.error} />}
			{task.error && <ErrorState error={task.error} />}
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
			{status === "failed" && (
				<ErrorState error={new Error(task.data?.error ?? "The run failed")} />
			)}
			{status === "done" && task.data?.risk_model && (
				<RiskModelResults data={task.data.risk_model} />
			)}
		</div>
	);
}
