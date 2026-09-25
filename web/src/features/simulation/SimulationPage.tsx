import type { SimulationModel } from "@/shared/api/types";
import { Button } from "@/shared/ui";
import { SimulationPathsChart } from "@/shared/viz/charts/SimulationPathsChart";
import { CalibratePanel } from "./CalibratePanel";
import { ParamsForm } from "./ParamsForm";
import { useSimulation } from "./useSimulation";

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

export default function SimulationPage() {
	const sim = useSimulation();
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
		data,
		chartPaths,
		nPaths,
		ttm,
		nSteps,
	} = sim;

	return (
		<div className="mx-auto flex max-w-4xl flex-col gap-6">
			<header className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">Simulation</h1>
				<p className="text-sm text-ink-secondary">
					Pick a dynamics, set parameters by hand or calibrate against a real
					ticker, and watch simulated paths draw in.
				</p>
			</header>

			<div className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4">
				<div className="flex flex-wrap items-center gap-4">
					<label className="flex flex-col gap-1 text-sm">
						<span className="text-2xs text-ink-muted uppercase">Dynamics</span>
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

			<SimulationPathsChart
				paths={chartPaths}
				revealCount={data ? revealCount : undefined}
				title={`${MODEL_NAMES[model]} — ${nPaths} simulated paths`}
				subtitle={`T = ${ttm} years, ${nSteps} steps`}
				isLoading={simulate.isPending}
				error={simulate.error}
			/>
		</div>
	);
}
