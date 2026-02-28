import type { BarrierKind, DigitalPayoffType, ExoticProductType, LookbackExtremum, LookbackStyle } from "./types";
import type { PricingHook } from "./usePricing";
import CommonFields from "./CommonFields";

/** Products priced with a closed-form analytic engine (no MC). */
const ANALYTIC_PRODUCTS = new Set<ExoticProductType>(["digital"]);

type Props = Pick<
	PricingHook,
	/* common */
	| "spot" | "setSpot"
	| "strike" | "setStrike"
	| "maturity" | "setMaturity"
	| "rate" | "setRate"
	| "dividend" | "setDividend"
	| "vol" | "setVol"
	| "isCall" | "setIsCall"
	| "isLiveVol"
	/* MC params (exotics default to MC) */
	| "nPaths" | "setNPaths"
	| "seed" | "setSeed"
	| "mcEpsilon" | "setMcEpsilon"
	/* exotic selectors */
	| "exoticProduct"
	/* barrier */
	| "barrierLevel" | "setBarrierLevel"
	| "barrierKind" | "setBarrierKind"
	| "rebate" | "setRebate"
	| "brownianBridge" | "setBrownianBridge"
	/* digital */
	| "digitalPayoff" | "setDigitalPayoff"
	| "cashAmount" | "setCashAmount"
	/* lookback */
	| "lookbackStyle" | "setLookbackStyle"
	| "lookbackExtremum" | "setLookbackExtremum"
	| "lookbackNSteps" | "setLookbackNSteps"
	/* basket */
	| "basketWeights" | "setBasketWeights"
	| "basketSpots" | "setBasketSpots"
	| "basketVols" | "setBasketVols"
	| "basketDividends" | "setBasketDividends"
	| "basketCorrelation" | "setBasketCorrelation"
	/* cliquet */
	| "cliquetFloorRate" | "setCliquetFloorRate"
	| "cliquetCapRate" | "setCliquetCapRate"
	| "cliquetResetPeriods" | "setCliquetResetPeriods"
>;

export default function ExoticFields(props: Props) {
	const {
		exoticProduct,
		/* barrier */
		barrierLevel, setBarrierLevel, barrierKind, setBarrierKind, rebate, setRebate,
		brownianBridge, setBrownianBridge,
		/* digital */
		digitalPayoff, setDigitalPayoff, cashAmount, setCashAmount,
		/* lookback */
		lookbackStyle, setLookbackStyle,
		lookbackExtremum, setLookbackExtremum,
		lookbackNSteps, setLookbackNSteps,
		/* basket */
		basketWeights, setBasketWeights, basketSpots, setBasketSpots, basketVols, setBasketVols,
		basketDividends, setBasketDividends, basketCorrelation, setBasketCorrelation,
		/* cliquet */
		cliquetFloorRate, setCliquetFloorRate, cliquetCapRate, setCliquetCapRate,
		cliquetResetPeriods, setCliquetResetPeriods,
		/* MC */
		nPaths, setNPaths, seed, setSeed, mcEpsilon, setMcEpsilon,
		/* common passthrough */
		...common
	} = props;

	const isAnalytic = ANALYTIC_PRODUCTS.has(exoticProduct);

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

			{/* Common market fields (spot, strike, maturity, rate, div, vol) */}
			<CommonFields {...common} />

			{/* ── Barrier ────────────────────────────────── */}
			{exoticProduct === "barrier" && (
				<>
					<label className="field">
						Barrier type
						<select value={barrierKind} onChange={(e) => setBarrierKind(e.target.value as BarrierKind)}>
							<option value="up-and-in">Up-and-In</option>
							<option value="up-and-out">Up-and-Out</option>
							<option value="down-and-in">Down-and-In</option>
							<option value="down-and-out">Down-and-Out</option>
						</select>
					</label>
					<label className="field">
						Barrier level
						<input type="number" step="1" value={barrierLevel} onChange={(e) => setBarrierLevel(Number(e.target.value))} />
					</label>
					<label className="field">
						Rebate
						<input type="number" step="0.01" value={rebate} onChange={(e) => setRebate(Number(e.target.value))} />
					</label>
					<label className="field">
						Monitoring
						<select value={brownianBridge ? "continuous" : "discrete"} onChange={(e) => setBrownianBridge(e.target.value === "continuous")}>
							<option value="continuous">Continuous (Brownian bridge)</option>
							<option value="discrete">Discrete (exact schedule)</option>
						</select>
					</label>
				</>
			)}

			{/* ── Digital ────────────────────────────────── */}
			{exoticProduct === "digital" && (
				<>
					<label className="field">
						Payoff type
						<select value={digitalPayoff} onChange={(e) => setDigitalPayoff(e.target.value as DigitalPayoffType)}>
							<option value="cash-or-nothing">Cash-or-Nothing</option>
							<option value="asset-or-nothing">Asset-or-Nothing</option>
						</select>
					</label>
					{digitalPayoff === "cash-or-nothing" && (
						<label className="field">
							Cash amount
							<input type="number" step="0.1" value={cashAmount} onChange={(e) => setCashAmount(Number(e.target.value))} />
						</label>
					)}
				</>
			)}

			{/* ── Lookback ───────────────────────────────── */}
			{exoticProduct === "lookback" && (
			<>
				<label className="field">
					Style
					<select value={lookbackStyle} onChange={(e) => setLookbackStyle(e.target.value as LookbackStyle)}>
						<option value="fixed-strike">Fixed-strike</option>
						<option value="floating-strike">Floating-strike</option>
					</select>
				</label>
				{lookbackStyle === "fixed-strike" && (
					<label className="field">
						Extremum
						<select value={lookbackExtremum} onChange={(e) => setLookbackExtremum(e.target.value as LookbackExtremum)}>
							<option value="maximum">Maximum (S_max)</option>
							<option value="minimum">Minimum (S_min)</option>
						</select>
					</label>
				)}
				<label className="field">
					Monitoring steps
					<input
						type="number"
						min={0}
						step={1}
						value={lookbackNSteps}
						onChange={(e) => setLookbackNSteps(Number(e.target.value))}
					/>
					<small>0 = auto (252 × T)</small>
				</label>
			</>
		)}

		{/* ── Basket ─────────────────────────────────── */}
			{exoticProduct === "basket" && (
				<>
					<label className="field span-2">
						Spots (comma-separated)
						<input type="text" value={basketSpots} onChange={(e) => setBasketSpots(e.target.value)} />
					</label>
					<label className="field span-2">
						Volatilities (comma-separated)
						<input type="text" value={basketVols} onChange={(e) => setBasketVols(e.target.value)} />
					</label>
					<label className="field span-2">
						Dividend yields (comma-separated)
						<input type="text" value={basketDividends} onChange={(e) => setBasketDividends(e.target.value)} />
					</label>
					<label className="field span-2">
						Weights (comma-separated)
						<input type="text" value={basketWeights} onChange={(e) => setBasketWeights(e.target.value)} />
						<small>Must sum to 1</small>
					</label>
					<label className="field">
						Pairwise correlation (ρ)
						<input
							type="number"
							min={-0.999}
							max={0.999}
							step={0.05}
							value={basketCorrelation}
							onChange={(e) => setBasketCorrelation(Number(e.target.value))}
						/>
						<small>Uniform pairwise ρ between all assets</small>
					</label>
				</>
			)}

			{/* ── Cliquet ────────────────────────────────── */}
			{exoticProduct === "cliquet" && (
				<>
					<label className="field">
						Floor rate
						<input type="number" step="0.01" value={cliquetFloorRate} onChange={(e) => setCliquetFloorRate(Number(e.target.value))} />
					</label>
					<label className="field">
						Cap rate
						<input type="number" step="0.01" value={cliquetCapRate} onChange={(e) => setCliquetCapRate(Number(e.target.value))} />
					</label>
					<label className="field">
						Reset periods
						<input type="number" step="1" value={cliquetResetPeriods} onChange={(e) => setCliquetResetPeriods(Number(e.target.value))} />
					</label>
				</>
			)}

			{/* ── MC engine params (only for MC-priced products) ─ */}
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
					<label className="field">
						MC epsilon
						<input type="number" step="0.0001" value={mcEpsilon} onChange={(e) => setMcEpsilon(Number(e.target.value))} />
					</label>
				</>
			)}
		</>
	);
}
