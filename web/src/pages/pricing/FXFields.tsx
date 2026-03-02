import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	| "spot" | "setSpot"
	| "strike" | "setStrike"
	| "maturity" | "setMaturity"
	| "vol" | "setVol"
	| "isCall" | "setIsCall"
	| "notional" | "setNotional"
	| "fxProduct"
	| "rateDomestic" | "setRateDomestic"
	| "rateForeign" | "setRateForeign"
>;

export default function FXFields(props: Props) {
	const {
		fxProduct,
		spot, setSpot, strike, setStrike, maturity, setMaturity,
		vol, setVol, isCall, setIsCall, notional, setNotional,
		rateDomestic, setRateDomestic, rateForeign, setRateForeign,
	} = props;

	return (
		<>
			<label className="field">
				Pricing method
				<input type="text" value="Analytic (closed-form)" readOnly className="input-readonly" />
			</label>

			{fxProduct === "fx-option" && (
				<label className="field">
					Option type
					<select value={isCall ? "call" : "put"} onChange={(e) => setIsCall(e.target.value === "call")}>
						<option value="call">Call</option>
						<option value="put">Put</option>
					</select>
				</label>
			)}

			<label className="field">
				Spot FX rate
				<input type="number" step="0.01" value={spot} onChange={(e) => setSpot(Number(e.target.value))} />
				<small>Domestic per unit foreign</small>
			</label>
			<label className="field">
				Strike
				<input type="number" step="0.01" value={strike} onChange={(e) => setStrike(Number(e.target.value))} />
			</label>
			<label className="field">
				Maturity (years)
				<input type="number" step="0.25" value={maturity} onChange={(e) => setMaturity(Number(e.target.value))} />

			</label>
			<label className="field">
				Domestic rate
				<input type="number" step="0.01" value={rateDomestic} onChange={(e) => setRateDomestic(Number(e.target.value))} />
			</label>
			<label className="field">
				Foreign rate
				<input type="number" step="0.01" value={rateForeign} onChange={(e) => setRateForeign(Number(e.target.value))} />
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
