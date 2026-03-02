import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	| "spot" | "setSpot"
	| "strike" | "setStrike"
	| "maturity" | "setMaturity"
	| "rate" | "setRate"
	| "vol" | "setVol"
	| "isCall" | "setIsCall"
	| "notional" | "setNotional"
	| "commodityProduct"
	| "storageCost" | "setStorageCost"
	| "convenienceYield" | "setConvenienceYield"
>;

export default function CommodityFields(props: Props) {
	const {
		commodityProduct,
		spot, setSpot, strike, setStrike, maturity, setMaturity,
		rate, setRate, vol, setVol, isCall, setIsCall, notional, setNotional,
		storageCost, setStorageCost, convenienceYield, setConvenienceYield,
	} = props;

	return (
		<>
			<label className="field">
				Pricing method
				<input type="text" value="Analytic (closed-form)" readOnly className="input-readonly" />
			</label>

			{commodityProduct === "commodity-option" && (
				<label className="field">
					Option type
					<select value={isCall ? "call" : "put"} onChange={(e) => setIsCall(e.target.value === "call")}>
						<option value="call">Call</option>
						<option value="put">Put</option>
					</select>
				</label>
			)}

			<label className="field">
				Spot price
				<input type="number" step="1" value={spot} onChange={(e) => setSpot(Number(e.target.value))} />
			</label>
			<label className="field">
				Strike
				<input type="number" step="1" value={strike} onChange={(e) => setStrike(Number(e.target.value))} />
			</label>
			<label className="field">
				Maturity (years)
				<input type="number" step="0.25" value={maturity} onChange={(e) => setMaturity(Number(e.target.value))} />
			</label>
			<label className="field">
				Risk-free rate
				<input type="number" step="0.01" value={rate} onChange={(e) => setRate(Number(e.target.value))} />
			</label>
			<label className="field">
				Storage cost
				<input type="number" step="0.01" value={storageCost} onChange={(e) => setStorageCost(Number(e.target.value))} />
				<small>Annual proportion of spot</small>
			</label>
			<label className="field">
				Convenience yield
				<input type="number" step="0.01" value={convenienceYield} onChange={(e) => setConvenienceYield(Number(e.target.value))} />
			</label>
			<label className="field">
				Volatility
				<input type="number" step="0.01" value={vol} onChange={(e) => setVol(Number(e.target.value))} />
			</label>
			<label className="field">
				Notional
				<input type="number" step="1" value={notional} onChange={(e) => setNotional(Number(e.target.value))} />
			</label>
		</>
	);
}
