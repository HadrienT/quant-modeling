import type { SimulationModel } from "@/shared/api/types";
import { Button } from "@/shared/ui";
import { SimulationPathsChart } from "@/shared/viz/charts/SimulationPathsChart";
import { CalibratePanel } from "./CalibratePanel";
import { ParamsForm } from "./ParamsForm";
import { useSimulation } from "./useSimulation";

/**
 * Simulation page: pick a dynamics (Black-Scholes or SABR — the two models
 * with a real market calibrator today, see CalibratePanel), set parameters
 * by hand, or calibrate against a real ticker's option chain, and watch
 * simulated paths draw themselves in.
 *
 * "Draw themselves in" is a client-side reveal animation (useSimulation's
 * playAnimation) over paths computed in one batch server-side
 * (engines/mc/path_simulation.hpp) — no streaming infra. Path generation
 * there is explicitly illustrative, not a pricing engine (SABR pricing
 * itself stays on Hagan's formula / the arbitrage-free PDE); this page is
 * for building intuition about a dynamics, not for pricing anything.
 */
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
								setModel(e.target.value as SimulationModel);
								setCalibrated(null);
							}}
						>
							<option value="black_scholes">Black-Scholes</option>
							<option value="sabr">SABR</option>
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
				title={`${model === "black_scholes" ? "Black-Scholes" : "SABR"} — ${nPaths} simulated paths`}
				subtitle={`T = ${ttm} years, ${nSteps} steps`}
				isLoading={simulate.isPending}
				error={simulate.error}
			/>
		</div>
	);
}
