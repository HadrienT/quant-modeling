import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	/* common */
	| "spot" | "setSpot"
	| "rate" | "setRate"
	| "dividend" | "setDividend"
	| "vol" | "setVol"
	| "maturity" | "setMaturity"
	| "notional" | "setNotional"
	| "nPaths" | "setNPaths"
	| "seed" | "setSeed"
	/* vol product selector */
	| "volProduct"
	/* variance swap */
	| "strikeVar" | "setStrikeVar"
	| "varSwapEngine" | "setVarSwapEngine"
	/* volatility swap */
	| "strikeVol" | "setStrikeVol"
	/* shared */
	| "volObsDates" | "setVolObsDates"
	/* dispersion */
	| "dispersionSpots" | "setDispersionSpots"
	| "dispersionVols" | "setDispersionVols"
	| "dispersionDividends" | "setDispersionDividends"
	| "dispersionWeights" | "setDispersionWeights"
	| "dispersionCorrelation" | "setDispersionCorrelation"
	| "strikeSpread" | "setStrikeSpread"
>;

export default function VolatilityFields(props: Props) {
	const {
		volProduct,
		spot, setSpot, rate, setRate, dividend, setDividend, vol, setVol,
		maturity, setMaturity, notional, setNotional,
		nPaths, setNPaths, seed, setSeed,
		strikeVar, setStrikeVar, varSwapEngine, setVarSwapEngine,
		strikeVol, setStrikeVol,
		volObsDates, setVolObsDates,
		dispersionSpots, setDispersionSpots, dispersionVols, setDispersionVols,
		dispersionDividends, setDispersionDividends, dispersionWeights, setDispersionWeights,
		dispersionCorrelation, setDispersionCorrelation,
		strikeSpread, setStrikeSpread,
	} = props;

	const isSingle = volProduct === "variance-swap" || volProduct === "volatility-swap";
	const isAnalytic = volProduct === "variance-swap" && varSwapEngine === "analytic";

	return (
		<>
			<label className="field">
				Pricing method
				<input
					type="text"
					value={isAnalytic ? "Analytic (closed-form)" : "Monte Carlo"}
					readOnly
					className="input-readonly"
				/>
			</label>

			{/* ── single‐asset fields (var/vol swap) ─── */}
			{isSingle && (
				<>
					<label className="field">
						Spot
						<input type="number" step="1" value={spot} onChange={(e) => setSpot(Number(e.target.value))} />
					</label>
					<label className="field">
						Maturity (years)
						<input type="number" step="0.25" value={maturity} onChange={(e) => setMaturity(Number(e.target.value))} />
					</label>
					<label className="field">
						Rate
						<input type="number" step="0.01" value={rate} onChange={(e) => setRate(Number(e.target.value))} />
					</label>
					<label className="field">
						Dividend
						<input type="number" step="0.01" value={dividend} onChange={(e) => setDividend(Number(e.target.value))} />
					</label>
					<label className="field">
						Volatility (σ)
						<input type="number" step="0.01" value={vol} onChange={(e) => setVol(Number(e.target.value))} />
					</label>
					<label className="field">
						Notional
						<input type="number" step="100" value={notional} onChange={(e) => setNotional(Number(e.target.value))} />
					</label>
				</>
			)}

			{/* ── Variance swap specific ───────────────── */}
			{volProduct === "variance-swap" && (
				<>
					<label className="field">
						Variance strike (K_var)
						<input type="number" step="0.001" value={strikeVar} onChange={(e) => setStrikeVar(Number(e.target.value))} />
						<small>Annualised (e.g. 0.04 = σ²=20%²)</small>
					</label>
					<label className="field">
						Engine
						<select value={varSwapEngine} onChange={(e) => setVarSwapEngine(e.target.value as "analytic" | "mc")}>
							<option value="analytic">Analytic</option>
							<option value="mc">Monte Carlo</option>
						</select>
					</label>
				</>
			)}

			{/* ── Volatility swap specific ─────────────── */}
			{volProduct === "volatility-swap" && (
				<label className="field">
					Vol strike (K_vol)
					<input type="number" step="0.01" value={strikeVol} onChange={(e) => setStrikeVol(Number(e.target.value))} />
					<small>Annualised (e.g. 0.20 = 20%)</small>
				</label>
			)}

			{/* ── Dispersion swap ──────────────────────── */}
			{volProduct === "dispersion-swap" && (
				<>
					<label className="field">
						Maturity (years)
						<input type="number" step="0.25" value={maturity} onChange={(e) => setMaturity(Number(e.target.value))} />
					</label>
					<label className="field">
						Rate
						<input type="number" step="0.01" value={rate} onChange={(e) => setRate(Number(e.target.value))} />
					</label>
					<label className="field">
						Notional
						<input type="number" step="100" value={notional} onChange={(e) => setNotional(Number(e.target.value))} />
					</label>
					<label className="field span-2">
						Spots (comma-separated)
						<input type="text" value={dispersionSpots} onChange={(e) => setDispersionSpots(e.target.value)} />
					</label>
					<label className="field span-2">
						Volatilities (comma-separated)
						<input type="text" value={dispersionVols} onChange={(e) => setDispersionVols(e.target.value)} />
					</label>
					<label className="field span-2">
						Dividend yields (comma-separated)
						<input type="text" value={dispersionDividends} onChange={(e) => setDispersionDividends(e.target.value)} />
					</label>
					<label className="field span-2">
						Weights (comma-separated)
						<input type="text" value={dispersionWeights} onChange={(e) => setDispersionWeights(e.target.value)} />
						<small>Must sum to 1</small>
					</label>
					<label className="field">
						Pairwise correlation (ρ)
						<input
							type="number"
							min={-0.999}
							max={0.999}
							step={0.05}
							value={dispersionCorrelation}
							onChange={(e) => setDispersionCorrelation(Number(e.target.value))}
						/>
					</label>
					<label className="field">
						Strike spread
						<input type="number" step="0.01" value={strikeSpread} onChange={(e) => setStrikeSpread(Number(e.target.value))} />
					</label>
				</>
			)}

			{/* ── Observation dates (shared) ───────────── */}
			<label className="field span-2">
				Observation dates (comma-separated, leave empty for auto)
				<input type="text" value={volObsDates} onChange={(e) => setVolObsDates(e.target.value)} />
			</label>

			{/* ── MC params ────────────────────────────── */}
			{!isAnalytic && (
				<>
					<label className="field">
						Paths (MC)
						<input type="number" step="10000" value={nPaths} onChange={(e) => setNPaths(Number(e.target.value))} />
					</label>
					<label className="field">
						Seed (MC)
						<input type="number" value={seed} onChange={(e) => setSeed(Number(e.target.value))} />
					</label>
				</>
			)}
		</>
	);
}
