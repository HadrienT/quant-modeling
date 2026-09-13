import { Field } from "@/shared/ui";
import type { UseSimulation } from "./useSimulation";

/** Manual parameter fields (model-specific + shared) — split out of
 * SimulationPage.tsx to stay under the per-feature-file line budget. */
export function ParamsForm(sim: UseSimulation) {
	const {
		model,
		spot,
		setSpot,
		rate,
		setRate,
		dividend,
		setDividend,
		vol,
		setVol,
		forward,
		setForward,
		alpha,
		setAlpha,
		beta,
		setBeta,
		rho,
		setRho,
		nu,
		setNu,
		ttm,
		setTtm,
		nSteps,
		setNSteps,
		nPaths,
		setNPaths,
		seed,
		setSeed,
	} = sim;

	return (
		<div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
			{model === "black_scholes" ? (
				<>
					<Field
						label="Spot"
						inputMode="decimal"
						value={spot}
						onChange={(e) => setSpot(e.target.value)}
					/>
					<Field
						label="Rate %"
						inputMode="decimal"
						value={rate}
						onChange={(e) => setRate(e.target.value)}
					/>
					<Field
						label="Dividend %"
						inputMode="decimal"
						value={dividend}
						onChange={(e) => setDividend(e.target.value)}
					/>
					<Field
						label="Vol %"
						inputMode="decimal"
						value={vol}
						onChange={(e) => setVol(e.target.value)}
					/>
				</>
			) : (
				<>
					<Field
						label="Forward"
						inputMode="decimal"
						value={forward}
						onChange={(e) => setForward(e.target.value)}
					/>
					<Field
						label="Alpha"
						inputMode="decimal"
						value={alpha}
						onChange={(e) => setAlpha(e.target.value)}
					/>
					<Field
						label="Beta"
						inputMode="decimal"
						value={beta}
						onChange={(e) => setBeta(e.target.value)}
					/>
					<Field
						label="Rho"
						inputMode="decimal"
						value={rho}
						onChange={(e) => setRho(e.target.value)}
					/>
					<Field
						label="Nu"
						inputMode="decimal"
						value={nu}
						onChange={(e) => setNu(e.target.value)}
					/>
				</>
			)}
			<Field
				label="Maturity (years)"
				inputMode="decimal"
				value={ttm}
				onChange={(e) => setTtm(e.target.value)}
			/>
			<Field
				label="Steps"
				inputMode="numeric"
				value={nSteps}
				onChange={(e) => setNSteps(e.target.value)}
			/>
			<Field
				label="Paths"
				inputMode="numeric"
				value={nPaths}
				onChange={(e) => setNPaths(e.target.value)}
			/>
			<Field
				label="Seed"
				inputMode="numeric"
				value={seed}
				onChange={(e) => setSeed(e.target.value)}
			/>
		</div>
	);
}
