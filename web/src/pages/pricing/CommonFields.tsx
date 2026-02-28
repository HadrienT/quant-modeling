import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	| "spot" | "setSpot"
	| "strike" | "setStrike"
	| "maturity" | "setMaturity"
	| "rate" | "setRate"
	| "dividend" | "setDividend"
	| "vol" | "setVol"
	| "isLiveVol"
>;

/** Shared market-data fields used by vanilla and exotic options. */
export default function CommonFields(props: Props) {
	const {
		spot, setSpot, strike, setStrike, maturity, setMaturity,
		rate, setRate, dividend, setDividend, vol, setVol,
		isLiveVol,
	} = props;

	return (
		<>

			{!isLiveVol && (
				<label className="field">
					Spot
					<input type="number" step="1" value={spot} onChange={(e) => setSpot(Number(e.target.value))} />
				</label>
			)}
			<label className="field">
				Strike
				<input type="number" step="1" value={strike} onChange={(e) => setStrike(Number(e.target.value))} />
			</label>
			<label className="field">
				Maturity (years)
				<input type="number" step="0.25" value={maturity} onChange={(e) => setMaturity(Number(e.target.value))} />
			</label>
			<label className="field">
				Rate
				<input type="number" step="0.01" value={rate} onChange={(e) => setRate(Number(e.target.value))} />
			</label>
			{!isLiveVol && (
				<label className="field">
					Dividend
					<input type="number" step="0.01" value={dividend} onChange={(e) => setDividend(Number(e.target.value))} />
				</label>
			)}
			{!isLiveVol && (
				<label className="field">
					Volatility
					<input type="number" step="0.01" value={vol} onChange={(e) => setVol(Number(e.target.value))} />
				</label>
			)}
		</>
	);
}
