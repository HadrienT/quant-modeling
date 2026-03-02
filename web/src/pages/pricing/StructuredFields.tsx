import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	/* common */
	| "spot" | "setSpot"
	| "strike" | "setStrike"
	| "rate" | "setRate"
	| "dividend" | "setDividend"
	| "vol" | "setVol"
	| "isCall" | "setIsCall"
	| "notional" | "setNotional"
	| "nPaths" | "setNPaths"
	| "seed" | "setSeed"
	/* structured selector */
	| "structuredProduct"
	/* autocall */
	| "autocallBarrier" | "setAutocallBarrier"
	| "couponBarrier" | "setCouponBarrier"
	| "putBarrier" | "setPutBarrier"
	| "autocallCouponRate" | "setAutocallCouponRate"
	| "memoryCoupon" | "setMemoryCoupon"
	| "kiContinuous" | "setKiContinuous"
	| "observationDates" | "setObservationDates"
	/* mountain */
	| "mountainSpots" | "setMountainSpots"
	| "mountainVols" | "setMountainVols"
	| "mountainDividends" | "setMountainDividends"
	| "mountainCorrelations" | "setMountainCorrelations"
	| "mountainObsDates" | "setMountainObsDates"
>;

export default function StructuredFields(props: Props) {
	const {
		structuredProduct,
		/* common */
		spot, setSpot, rate, setRate, dividend, setDividend, vol, setVol,
		isCall, setIsCall, notional, setNotional,
		strike, setStrike,
		nPaths, setNPaths, seed, setSeed,
		/* autocall */
		autocallBarrier, setAutocallBarrier,
		couponBarrier, setCouponBarrier,
		putBarrier, setPutBarrier,
		autocallCouponRate, setAutocallCouponRate,
		memoryCoupon, setMemoryCoupon,
		kiContinuous, setKiContinuous,
		observationDates, setObservationDates,
		/* mountain */
		mountainSpots, setMountainSpots,
		mountainVols, setMountainVols,
		mountainDividends, setMountainDividends,
		mountainCorrelations, setMountainCorrelations,
		mountainObsDates, setMountainObsDates,
	} = props;

	return (
		<>
			<label className="field">
				Pricing method
				<input type="text" value="Monte Carlo" readOnly className="input-readonly" />
			</label>

			{/* ── Autocall ─────────────────────────────── */}
			{structuredProduct === "autocall" && (
				<>
					<label className="field">
						Spot
						<input type="number" step="1" value={spot} onChange={(e) => setSpot(Number(e.target.value))} />
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
						Volatility
						<input type="number" step="0.01" value={vol} onChange={(e) => setVol(Number(e.target.value))} />
					</label>
					<label className="field">
						Notional
						<input type="number" step="100" value={notional} onChange={(e) => setNotional(Number(e.target.value))} />
					</label>
					<label className="field">
						Autocall barrier
						<input type="number" step="0.05" value={autocallBarrier} onChange={(e) => setAutocallBarrier(Number(e.target.value))} />
						<small>Fraction of spot (e.g. 1.0 = 100%)</small>
					</label>
					<label className="field">
						Coupon barrier
						<input type="number" step="0.05" value={couponBarrier} onChange={(e) => setCouponBarrier(Number(e.target.value))} />
					</label>
					<label className="field">
						Put barrier (KI)
						<input type="number" step="0.05" value={putBarrier} onChange={(e) => setPutBarrier(Number(e.target.value))} />
					</label>
					<label className="field">
						Coupon rate
						<input type="number" step="0.01" value={autocallCouponRate} onChange={(e) => setAutocallCouponRate(Number(e.target.value))} />
					</label>
					<label className="field span-2">
						Observation dates (comma-separated, in years)
						<input type="text" value={observationDates} onChange={(e) => setObservationDates(e.target.value)} />
					</label>
					<label className="field">
						Memory coupon
						<select value={memoryCoupon ? "yes" : "no"} onChange={(e) => setMemoryCoupon(e.target.value === "yes")}>
							<option value="yes">Yes</option>
							<option value="no">No</option>
						</select>
					</label>
					<label className="field">
						Knock-in monitoring
						<select value={kiContinuous ? "continuous" : "discrete"} onChange={(e) => setKiContinuous(e.target.value === "continuous")}>
							<option value="discrete">Discrete</option>
							<option value="continuous">Continuous</option>
						</select>
					</label>
				</>
			)}

			{/* ── Mountain / Himalaya ──────────────────── */}
			{structuredProduct === "mountain" && (
				<>
					<label className="field">
						Option type
						<select value={isCall ? "call" : "put"} onChange={(e) => setIsCall(e.target.value === "call")}>
							<option value="call">Call</option>
							<option value="put">Put</option>
						</select>
					</label>
					<label className="field">
						Strike
						<input type="number" step="1" value={strike} onChange={(e) => setStrike(Number(e.target.value))} />
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
						<input type="text" value={mountainSpots} onChange={(e) => setMountainSpots(e.target.value)} />
					</label>
					<label className="field span-2">
						Volatilities (comma-separated)
						<input type="text" value={mountainVols} onChange={(e) => setMountainVols(e.target.value)} />
					</label>
					<label className="field span-2">
						Dividend yields (comma-separated)
						<input type="text" value={mountainDividends} onChange={(e) => setMountainDividends(e.target.value)} />
					</label>
					<label className="field span-2">
						Correlations (row-major, comma-separated, leave empty for identity)
						<input type="text" value={mountainCorrelations} onChange={(e) => setMountainCorrelations(e.target.value)} />
						<small>E.g. for 3 assets: 1,0.5,0.3,0.5,1,0.4,0.3,0.4,1</small>
					</label>
					<label className="field span-2">
						Observation dates (comma-separated, in years)
						<input type="text" value={mountainObsDates} onChange={(e) => setMountainObsDates(e.target.value)} />
					</label>
				</>
			)}

			{/* ── MC params ────────────────────────────── */}
			<label className="field">
				Paths (MC)
				<input type="number" step="10000" value={nPaths} onChange={(e) => setNPaths(Number(e.target.value))} />
			</label>
			<label className="field">
				Seed (MC)
				<input type="number" value={seed} onChange={(e) => setSeed(Number(e.target.value))} />
			</label>
		</>
	);
}
