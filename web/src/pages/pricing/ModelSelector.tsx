import type { ModelType, VolSourceType } from "./types";
import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	| "model" | "setModel"
	| "engine"
	| "volSource" | "setVolSource"
	| "ticker" | "setTicker"
	| "isDupire" | "isLiveVol"
	| "lvNStepsPerYear" | "setLvNStepsPerYear"
>;

/**
 * Model selector: Black-Scholes (flat vol) or Dupire Local-Vol.
 * When Dupire is selected, a toggle lets you choose between:
 *   "real" — enter a stock ticker, IV surface is fetched live
 *   "manual" — enter all vol parameters by hand
 */
export default function ModelSelector(props: Props) {
	const {
		model, setModel,
		engine,
		volSource, setVolSource,
		ticker, setTicker,
		isDupire, isLiveVol,
		lvNStepsPerYear, setLvNStepsPerYear,
	} = props;

	/* Analytic engines assume flat vol — lock model to Black-Scholes */
	const analyticLocked = engine === "analytic";

	return (
		<>
			<label className="field">
				Model
				<select
					value={analyticLocked ? "black-scholes" : model}
					disabled={analyticLocked}
					onChange={(e) => setModel(e.target.value as ModelType)}
				>
					<option value="black-scholes">Black-Scholes (flat vol)</option>
					{!analyticLocked && (
						<option value="dupire-local-vol">Dupire Local Vol</option>
					)}
				</select>
			</label>

			{isDupire && (
				<>
					<label className="field">
						Volatility source
						<div className="vol-source-toggle">
							<button
								type="button"
								className={`vol-source-btn${volSource === "real" ? " active" : ""}`}
								onClick={() => setVolSource("real" as VolSourceType)}
							>
								Real data
							</button>
							<button
								type="button"
								className={`vol-source-btn${volSource === "manual" ? " active" : ""}`}
								onClick={() => setVolSource("manual" as VolSourceType)}
							>
								Manual input
							</button>
						</div>
					</label>

					{isLiveVol && (
						<>
							<label className="field">
								Stock ticker
								<input
									type="text"
									placeholder="e.g. AAPL, TSLA, SPY"
									value={ticker}
									onChange={(e) => setTicker(e.target.value.toUpperCase())}
								/>
								<small>
									The IV surface will be extracted from this stock's option chain
								</small>
							</label>
							<label className="field">
								Steps per year
								<input
									type="number"
									min={12}
									max={2520}
									step={1}
									value={lvNStepsPerYear}
									onChange={(e) => setLvNStepsPerYear(Number(e.target.value))}
								/>
								<small>Euler-Maruyama discretization (default: 252)</small>
							</label>
						</>
					)}
				</>
			)}
		</>
	);
}
