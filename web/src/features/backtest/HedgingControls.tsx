import { useState } from "react";
import { Button, Field } from "@/shared/ui";
import {
	DEFAULT_FORM,
	HEDGES,
	type HedgeId,
	type HedgingForm,
	maxPaths,
	type WorldKind,
} from "./hedging";

const WORLDS: { id: WorldKind; label: string }[] = [
	{ id: "heston", label: "Heston" },
	{ id: "ticker", label: "Heston calibrated on a ticker" },
	{ id: "black_scholes", label: "Black-Scholes" },
];

/**
 * The hedging backtest's inputs: the world the paths follow (typed, or a
 * Heston world fitted to a ticker's stored option chain), the option, the
 * hedges compared on the same paths, and the effort.
 */
export function HedgingControls({
	running,
	onRun,
}: {
	running: boolean;
	onRun: (form: HedgingForm) => void;
}) {
	const [f, setF] = useState<HedgingForm>(DEFAULT_FORM);
	const set = (key: keyof HedgingForm) => (e: { target: { value: string } }) =>
		setF((prev) => ({ ...prev, [key]: e.target.value }));
	const toggle = (id: HedgeId) =>
		setF((prev) => ({
			...prev,
			hedges: prev.hedges.includes(id)
				? prev.hedges.filter((h) => h !== id)
				: [...prev.hedges, id],
		}));
	const cap = maxPaths(f.world);
	const tooMany = Number(f.paths) > cap;
	const field = (key: keyof HedgingForm, label: string, title?: string) => (
		<Field
			label={label}
			inputMode="decimal"
			value={String(f[key])}
			onChange={set(key)}
			title={title}
		/>
	);

	return (
		<div className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4">
			<div className="flex flex-wrap items-end gap-3">
				<label className="flex flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">World</span>
					<select
						className="rounded border border-hairline bg-surface px-2 py-1"
						value={f.world}
						onChange={(e) =>
							setF((prev) => ({ ...prev, world: e.target.value as WorldKind }))
						}
					>
						{WORLDS.map((w) => (
							<option key={w.id} value={w.id}>
								{w.label}
							</option>
						))}
					</select>
				</label>
				{f.world === "ticker" && field("ticker", "Ticker")}
				{f.world !== "ticker" && field("spot", "Spot")}
				{field("ratePct", "Rate %", "Continuous, per year")}
				{f.world !== "ticker" && field("dividendPct", "Dividend %")}
			</div>
			{f.world === "black_scholes" && (
				<div className="grid grid-cols-2 gap-3 sm:grid-cols-5">
					{field("volPct", "Vol %")}
				</div>
			)}
			{f.world === "heston" && (
				<div className="grid grid-cols-2 gap-3 sm:grid-cols-5">
					{field("v0", "v0", "Variance today")}
					{field("kappa", "kappa", "Mean reversion")}
					{field("theta", "theta", "Long-run variance")}
					{field("xi", "xi", "Vol of vol")}
					{field("rho", "rho", "Spot / vol correlation")}
				</div>
			)}
			<div className="grid grid-cols-2 gap-3 sm:grid-cols-4 lg:grid-cols-7">
				{field("strikePct", "Strike % of spot")}
				{field("maturity", "Maturity (years)")}
				<label className="flex flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">Option</span>
					<select
						className="rounded border border-hairline bg-surface px-2 py-1"
						value={f.isCall ? "call" : "put"}
						onChange={(e) =>
							setF((prev) => ({ ...prev, isCall: e.target.value === "call" }))
						}
					>
						<option value="call">Call, bought</option>
						<option value="put">Put, bought</option>
					</select>
				</label>
				{field("rebalances", "Rebalancings")}
				{field("paths", `Paths (≤ ${cap.toLocaleString("en-US")})`)}
				{field("costBps", "Cost (bp)", "Proportional cost of each hedge trade")}
				{field("seed", "Seed")}
			</div>
			<fieldset className="flex flex-col gap-1.5">
				<legend className="mb-1 text-2xs text-ink-muted uppercase">
					Hedges compared, on the same paths
				</legend>
				<div className="flex flex-wrap gap-x-5 gap-y-1.5">
					{HEDGES.map((h) => (
						<label
							key={h.id}
							className="flex items-center gap-1.5 text-sm text-ink-secondary"
							title={h.help}
						>
							<input
								type="checkbox"
								checked={f.hedges.includes(h.id)}
								onChange={() => toggle(h.id)}
							/>
							{h.label}
						</label>
					))}
				</div>
			</fieldset>
			<div className="flex flex-wrap items-center gap-3">
				<Button
					onClick={() => onRun(f)}
					disabled={running || f.hedges.length === 0 || tooMany}
				>
					{running ? "Running…" : "Run the hedging backtest"}
				</Button>
				<span className="text-xs text-ink-muted">
					{tooMany
						? `At most ${cap.toLocaleString("en-US")} paths in this world.`
						: f.world === "black_scholes"
							? "A Black-Scholes world runs in about a second."
							: "A Heston world prices the option on every path at every date: a minute or more. It runs as a task you can follow and cancel."}
				</span>
			</div>
		</div>
	);
}
