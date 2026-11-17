import { useState } from "react";
import { Play } from "lucide-react";
import type { BacktestUniverse } from "@/shared/api";
import { Button, Field, Segmented } from "@/shared/ui";
import { AssetsField } from "./AssetsField";
import { type BacktestView, METHODS, isBounded, problems } from "./state";

/**
 * What is backtested and under which assumptions. Nothing here has a hidden
 * default: the cost of a trade, the estimation window, the rebalancing rule
 * and the three inputs of the bootstrap are fields, sent with the request and
 * recorded with the run. The form is a draft; "Run" puts it in the URL.
 */
export function BacktestControls({
	universe,
	view,
	running,
	onRun,
}: {
	universe: BacktestUniverse;
	view: BacktestView;
	running: boolean;
	onRun: (view: BacktestView) => void;
}) {
	const [draft, setDraft] = useState(view);
	const set = (patch: Partial<BacktestView>) =>
		setDraft((d) => ({ ...d, ...patch }));
	const number =
		(key: keyof BacktestView) => (e: { target: { value: string } }) =>
			set({ [key]: Number(e.target.value) });
	const errors = problems(draft, universe.last);

	return (
		<div className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4">
			<AssetsField universe={universe} draft={draft} set={set} />

			<div className="grid grid-cols-2 gap-3 md:grid-cols-4">
				<Field
					label="Start"
					type="date"
					min={universe.first}
					max={universe.last}
					value={draft.start}
					onChange={(e) => set({ start: e.target.value })}
				/>
				<Field
					label="End"
					type="date"
					min={universe.first}
					max={universe.last}
					value={draft.end || universe.last}
					onChange={(e) => set({ end: e.target.value })}
				/>
				<Field
					label="Estimation window"
					hint="trading days before each decision"
					type="number"
					min={20}
					max={1260}
					value={draft.lookback}
					onChange={number("lookback")}
				/>
				<Field
					label="Rebalance every"
					hint="trading days; 0 = first day of each month"
					type="number"
					min={0}
					max={252}
					value={draft.rebalanceDays}
					onChange={number("rebalanceDays")}
				/>
				<Field
					label="Cost of a trade"
					hint="basis points of the amount traded"
					type="number"
					min={0}
					max={500}
					step={0.5}
					value={draft.costBps}
					onChange={number("costBps")}
				/>
				<Field
					label="Cap per asset"
					hint={isBounded(draft.method) ? "percent" : "not used by this method"}
					type="number"
					min={1}
					max={100}
					disabled={!isBounded(draft.method)}
					value={draft.maxWeightPct}
					onChange={number("maxWeightPct")}
				/>
				<Field
					label="Bootstrap replications"
					hint="0 = analytical intervals only"
					type="number"
					min={0}
					max={10000}
					step={500}
					value={draft.replications}
					onChange={number("replications")}
				/>
				<Field
					label="Mean block length"
					hint="days, stationary bootstrap"
					type="number"
					min={1}
					max={250}
					value={draft.blockLength}
					onChange={number("blockLength")}
				/>
				<Field
					label="Seed"
					hint="of the bootstrap"
					type="number"
					min={0}
					value={draft.seed}
					onChange={number("seed")}
				/>
				<Field
					label="Newey-West lags"
					hint="factor regression; 0 = White"
					type="number"
					min={0}
					max={60}
					value={draft.lags}
					onChange={number("lags")}
				/>
			</div>

			<div className="flex flex-wrap items-center gap-x-6 gap-y-3 text-xs text-ink-secondary">
				<div className="flex items-center gap-2">
					<span>Weights</span>
					<Segmented
						label="Allocation method"
						options={METHODS}
						value={draft.method}
						onChange={(method) => set({ method })}
					/>
				</div>
				<div className="flex items-center gap-2">
					<span>Execution</span>
					<Segmented
						label="Execution timing"
						options={[
							{ value: "next_close", label: "Next close" },
							{ value: "next_open", label: "Next open" },
						]}
						value={draft.timing}
						onChange={(timing) => set({ timing })}
					/>
				</div>
				<div className="flex items-center gap-2">
					<span>Factors</span>
					<Segmented
						label="Factor model"
						options={[
							{ value: "ff3", label: "Fama-French 3" },
							{ value: "ff5", label: "Fama-French 5" },
						]}
						value={draft.factors}
						onChange={(factors) => set({ factors })}
					/>
				</div>
			</div>

			{errors.length > 0 && (
				<ul className="text-xs text-critical">
					{errors.map((e) => (
						<li key={e}>{e}</li>
					))}
				</ul>
			)}
			<div>
				<Button
					disabled={errors.length > 0 || running}
					onClick={() => onRun(draft)}
				>
					<Play className="size-3.5" />
					{running ? "Running…" : "Run backtest"}
				</Button>
			</div>
		</div>
	);
}
