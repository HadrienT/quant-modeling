import { useState } from "react";
import { Play, X } from "lucide-react";
import type { BacktestUniverse } from "@/shared/api";
import { Badge, Button, Combobox, Field, Label, Segmented } from "@/shared/ui";
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
	const funds = new Set(universe.etfs.map((a) => a.ticker));
	const stocks = draft.tickers.filter((t) => !funds.has(t));
	const offered = [...universe.etfs, ...universe.stocks]
		.filter((a) => !draft.tickers.includes(a.ticker))
		.map((a) => ({ value: a.ticker, label: `${a.ticker} · ${a.name}` }));

	return (
		<div className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4">
			<div>
				<Label>Assets</Label>
				<div className="mt-1 flex flex-wrap items-center gap-1.5">
					{draft.tickers.map((t) => (
						<Badge key={t} tone={funds.has(t) ? "accent" : "warning"}>
							{t}
							<button
								type="button"
								aria-label={`Remove ${t}`}
								onClick={() =>
									set({ tickers: draft.tickers.filter((x) => x !== t) })
								}
							>
								<X className="size-3" />
							</button>
						</Badge>
					))}
					<div className="w-56">
						<Combobox
							options={offered}
							value={null}
							onChange={(t) => set({ tickers: [...draft.tickers, t] })}
							placeholder="Add a fund or a stock…"
						/>
					</div>
				</div>
				{stocks.length > 0 && (
					<p className="mt-1.5 text-2xs text-warning">
						{stocks.join(", ")}: single stocks are picked with hindsight, and
						the database only has the companies that are still listed. A basket
						of funds does not carry that bias.
					</p>
				)}
			</div>

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
