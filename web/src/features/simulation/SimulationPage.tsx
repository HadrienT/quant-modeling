import { useState } from "react";
import type { SimulationModel } from "@/shared/api/types";
import { Button } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { CalibratePanel } from "./CalibratePanel";
import { ComparePanel } from "./ComparePanel";
import { DiagnosticsTiles } from "./DiagnosticsTiles";
import { DistributionPanel } from "./DistributionPanel";
import { ParamsForm } from "./ParamsForm";
import { SmilePanel } from "./SmilePanel";
import { useComparison } from "./useComparison";
import { useSimulation } from "./useSimulation";
import { VolPanel } from "./VolPanel";

/**
 * Simulation page: pick a dynamics (Black-Scholes, SABR, local vol, Heston,
 * stochastic-local vol), set parameters by hand or calibrate against a
 * ticker's option chain (see CalibratePanel; local vol and SLV only from a
 * ticker's stored surface), and watch simulated paths draw themselves in.
 *
 * "Draw themselves in" is a client-side reveal animation (useSimulation's
 * playAnimation) over paths computed in one batch server-side
 * (engines/mc/path_simulation.hpp) — no streaming infra. Path generation
 * there is explicitly illustrative, not a pricing engine (SABR pricing
 * itself stays on Hagan's formula / the arbitrage-free PDE); this page is
 * for building intuition about a dynamics, not for pricing anything.
 */
const MODEL_NAMES: Record<SimulationModel, string> = {
	black_scholes: "Black-Scholes",
	sabr: "SABR",
	local_vol: "Local vol (Dupire)",
	heston: "Heston",
	slv: "Stochastic-local vol",
};

const VIEWS = [
	["single", "One model"],
	["compare", "Local vol vs stochastic vol"],
] as const;

export default function SimulationPage() {
	const sim = useSimulation();
	const cmp = useComparison();
	const [view, setView] = useState<"single" | "compare">("single");
	const {
		model,
		setModel,
		mode,
		setMode,
		setCalibrated,
		simulate,
		runSimulation,
		playAnimation,
		revealCount,
		cursor,
		setCursor,
		data,
	} = sim;

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-6">
			<header className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">Simulation</h1>
				<p className="text-sm text-ink-secondary">
					Pick a dynamics, set parameters by hand or calibrate against a real
					ticker, and read what the paths say: the law of the spot, its
					volatility, the smile it implies against the market&apos;s.
				</p>
			</header>

			<div className="flex gap-1 self-start rounded-md border border-hairline p-0.5">
				{VIEWS.map(([key, label]) => (
					<button
						key={key}
						type="button"
						className={`rounded px-3 py-1 text-sm ${view === key ? "bg-accent text-white" : "text-ink-secondary"}`}
						onClick={() => setView(key)}
					>
						{label}
					</button>
				))}
			</div>

			{view === "compare" ? (
				<ComparePanel sim={sim} cmp={cmp} />
			) : (
				<>
					<div className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4">
						<div className="flex flex-wrap items-center gap-4">
							<label className="flex flex-col gap-1 text-sm">
								<span className="text-2xs text-ink-muted uppercase">
									Dynamics
								</span>
								<select
									className="rounded border border-hairline bg-surface px-2 py-1"
									value={model}
									onChange={(e) => {
										const next = e.target.value as SimulationModel;
										setModel(next);
										setCalibrated(null);
										// Their surface is the ticker's: nothing to type by hand.
										if (next === "local_vol" || next === "slv")
											setMode("calibrate");
									}}
								>
									{Object.entries(MODEL_NAMES).map(([key, label]) => (
										<option key={key} value={key}>
											{label}
										</option>
									))}
								</select>
							</label>

							<div className="flex gap-1 rounded-md border border-hairline p-0.5">
								<button
									type="button"
									className={`rounded px-3 py-1 text-sm ${
										mode === "manual"
											? "bg-accent text-white"
											: "text-ink-secondary"
									}`}
									onClick={() => setMode("manual")}
									disabled={model === "local_vol" || model === "slv"}
									title={
										model === "local_vol" || model === "slv"
											? "Calibrated on a ticker's stored option chain only"
											: undefined
									}
								>
									Manual
								</button>
								<button
									type="button"
									className={`rounded px-3 py-1 text-sm ${
										mode === "calibrate"
											? "bg-accent text-white"
											: "text-ink-secondary"
									}`}
									onClick={() => setMode("calibrate")}
								>
									Calibrate on ticker
								</button>
							</div>
						</div>

						{mode === "calibrate" && <CalibratePanel {...sim} />}

						<ParamsForm {...sim} />

						<div className="flex items-center gap-3">
							<Button onClick={runSimulation} disabled={simulate.isPending}>
								{simulate.isPending ? "Simulating…" : "Simulate"}
							</Button>
							{data && (
								<Button
									variant="secondary"
									onClick={() => playAnimation(data.time_grid.length - 1)}
								>
									Replay
								</Button>
							)}
						</div>
					</div>

					{simulate.error && <ErrorState error={simulate.error} />}
					{data && (
						<>
							<DiagnosticsTiles data={data} />
							<DistributionPanel
								data={data}
								title={`${MODEL_NAMES[data.model]}: the law of the spot`}
								revealCount={revealCount}
								cursor={cursor}
								setCursor={setCursor}
							/>
							<div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
								<VolPanel
									data={data}
									revealCount={revealCount}
									cursor={cursor}
									setCursor={setCursor}
								/>
								<SmilePanel key={data.time_grid.length} data={data} />
							</div>
						</>
					)}
				</>
			)}
		</div>
	);
}
