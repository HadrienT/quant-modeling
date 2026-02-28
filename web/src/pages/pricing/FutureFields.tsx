import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	| "spot" | "setSpot"
	| "strike" | "setStrike"
	| "maturity" | "setMaturity"
	| "rate" | "setRate"
	| "dividend" | "setDividend"
	| "notional" | "setNotional"
	| "fairForward"
>;

export default function FutureFields(props: Props) {
	const {
		spot, setSpot, strike, setStrike,
		maturity, setMaturity, rate, setRate,
		dividend, setDividend, notional, setNotional,
		fairForward,
	} = props;

	return (
		<>
			<label className="field">
				Spot
				<input type="number" step="1" value={spot} onChange={(e) => setSpot(Number(e.target.value))} />
			</label>
			<label className="field">
				Strike
				<input type="number" step="1" value={strike} onChange={(e) => setStrike(Number(e.target.value))} />
			</label>
			<div style={{ display: "flex", alignItems: "center", gap: 12, gridColumn: "1 / -1" }}>
				<button
					className="button secondary"
					type="button"
					disabled={!Number.isFinite(fairForward)}
					onClick={() => setStrike(Number(fairForward.toFixed(4)))}
				>
					Set strike to fair forward
				</button>
				<span className="price-muted">
					Fair forward: {Number.isFinite(fairForward) ? fairForward.toFixed(4) : "-"}
				</span>
			</div>
			<label className="field">
				Maturity (years)
				<input type="number" step="0.01" value={maturity} onChange={(e) => setMaturity(Number(e.target.value))} />
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
				Notional
				<input type="number" step="1" value={notional} onChange={(e) => setNotional(Number(e.target.value))} />
			</label>
		</>
	);
}
