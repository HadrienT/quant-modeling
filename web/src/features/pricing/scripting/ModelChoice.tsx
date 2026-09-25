import type { ModelRecommendation, ScriptModel } from "@/shared/api";
import { Field } from "@/shared/ui";
import { MODEL_LABELS } from "./modelLabels";

export type { ScriptModel };

/** Which dynamics the script is priced under. A script only describes a
 * payoff; this is the other half. "Auto" reads what the price depends on off
 * the script and picks the simplest model that captures it; every market
 * model calibrates the ticker's stored option chain. */
export function ModelChoice(props: {
	model: ScriptModel;
	onModel: (m: ScriptModel) => void;
	ticker: string;
	onTicker: (t: string) => void;
	/** From the last "Validate": what auto would pick for this script. */
	recommendation?: ModelRecommendation | null;
}) {
	const rec = props.model === "auto" ? props.recommendation : null;
	return (
		<div className="flex flex-col gap-2">
			<div className="grid grid-cols-2 gap-4 sm:grid-cols-3">
				<label className="col-span-2 flex flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">Model</span>
					<select
						className="rounded border border-hairline bg-surface px-2 py-1"
						value={props.model}
						onChange={(e) => props.onModel(e.target.value as ScriptModel)}
					>
						{(Object.keys(MODEL_LABELS) as ScriptModel[]).map((m) => (
							<option key={m} value={m}>
								{MODEL_LABELS[m]}
							</option>
						))}
					</select>
				</label>
				{props.model !== "black_scholes" && (
					<Field
						label="Ticker"
						value={props.ticker}
						onChange={(e) => props.onTicker(e.target.value.toUpperCase())}
					/>
				)}
			</div>
			{rec && (
				<p className="text-2xs text-ink-secondary">
					Auto will price this script under{" "}
					<span className="font-medium text-ink">
						{MODEL_LABELS[rec.model]}
					</span>
					: {rec.reason}
				</p>
			)}
		</div>
	);
}
