import type { PricingHook } from "./usePricing";
import CommonFields from "./CommonFields";

type Props = Pick<
	PricingHook,
	| "product"
	| "isCall" | "setIsCall"
	| "engine" | "setEngine"
	| "averageType" | "setAverageType"
	| "spot" | "setSpot"
	| "strike" | "setStrike"
	| "maturity" | "setMaturity"
	| "rate" | "setRate"
	| "dividend" | "setDividend"
	| "vol" | "setVol"
	| "isLiveVol"
	| "nPaths" | "setNPaths"
	| "seed" | "setSeed"
	| "mcEpsilon" | "setMcEpsilon"
	| "treeSteps" | "setTreeSteps"
	| "pdeSpaceSteps" | "setPdeSpaceSteps"
	| "pdeTimeSteps" | "setPdeTimeSteps"
>;

export default function OptionFields(props: Props) {
	const {
		product, engine, setEngine,
		averageType, setAverageType,
		nPaths, setNPaths, seed, setSeed, mcEpsilon, setMcEpsilon,
		treeSteps, setTreeSteps, pdeSpaceSteps, setPdeSpaceSteps,
		pdeTimeSteps, setPdeTimeSteps,
		...common
	} = props;

	return (
		<>

			{product === "vanilla" && (
				<label className="field">
					Pricing engine
					<select value={engine} onChange={(e) => setEngine(e.target.value as typeof engine)}>
						<option value="analytic">Analytic (Black-Scholes)</option>
						<option value="mc">Monte Carlo</option>
						<option value="binomial">Binomial Tree</option>
						<option value="trinomial">Trinomial Tree</option>
						<option value="pde">PDE Crank-Nicolson</option>
					</select>
				</label>
			)}
			{product === "american" && (
				<label className="field">
					Pricing engine
					<select value={engine} onChange={(e) => setEngine(e.target.value as typeof engine)}>
						<option value="binomial">Binomial Tree</option>
						<option value="trinomial">Trinomial Tree</option>
					</select>
				</label>
			)}
			{product === "asian" && (
				<>
					<label className="field">
						Pricing engine
						<select value={engine} onChange={(e) => setEngine(e.target.value as typeof engine)}>
							<option value="analytic">Analytic</option>
							<option value="mc">Monte Carlo</option>
						</select>
					</label>
					<label className="field">
						Average type
						<select value={averageType} onChange={(e) => setAverageType(e.target.value as typeof averageType)}>
							<option value="arithmetic">Arithmetic</option>
							<option value="geometric">Geometric</option>
						</select>
					</label>
				</>
			)}

			{/* Shared market-data fields */}
			<CommonFields {...common} />

			{engine === "mc" && (
				<>
					<label className="field">
						Paths (MC)
						<input type="number" step="10000" value={nPaths} onChange={(e) => setNPaths(Number(e.target.value))} />
					</label>
					<label className="field">
						Seed (MC)
						<input type="number" value={seed} onChange={(e) => setSeed(Number(e.target.value))} />
					</label>
					<label className="field">
						MC epsilon
						<input type="number" step="0.0001" value={mcEpsilon} onChange={(e) => setMcEpsilon(Number(e.target.value))} />
					</label>
				</>
			)}

			{product === "vanilla" && (engine === "binomial" || engine === "trinomial") && (
				<label className="field">
					Tree steps
					<input type="number" step="10" value={treeSteps} onChange={(e) => setTreeSteps(Number(e.target.value))} />
				</label>
			)}
			{product === "vanilla" && engine === "pde" && (
				<>
					<label className="field">
						PDE space steps
						<input type="number" step="10" value={pdeSpaceSteps} onChange={(e) => setPdeSpaceSteps(Number(e.target.value))} />
					</label>
					<label className="field">
						PDE time steps
						<input type="number" step="10" value={pdeTimeSteps} onChange={(e) => setPdeTimeSteps(Number(e.target.value))} />
					</label>
				</>
			)}
			{product === "american" && (
				<label className="field">
					Tree steps
					<input type="number" step="10" value={treeSteps} onChange={(e) => setTreeSteps(Number(e.target.value))} />
				</label>
			)}
		</>
	);
}
