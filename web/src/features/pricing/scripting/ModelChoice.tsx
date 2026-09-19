import { Field } from "@/shared/ui";

export type ScriptModel = "black_scholes" | "local_vol";

/** Which dynamics the script is priced under. A script only describes a
 * payoff; this is the other half. Local vol calibrates the ticker's option
 * chain into a Dupire surface, so skew is priced. */
export function ModelChoice(props: {
	model: ScriptModel;
	onModel: (m: ScriptModel) => void;
	ticker: string;
	onTicker: (t: string) => void;
}) {
	return (
		<div className="grid grid-cols-2 gap-4 sm:grid-cols-3">
			<label className="flex flex-col gap-1 text-sm">
				<span className="text-2xs text-ink-muted uppercase">Model</span>
				<select
					className="rounded border border-hairline bg-surface px-2 py-1"
					value={props.model}
					onChange={(e) => props.onModel(e.target.value as ScriptModel)}
				>
					<option value="black_scholes">Black-Scholes (flat vol)</option>
					<option value="local_vol">Local vol (market surface)</option>
				</select>
			</label>
			{props.model === "local_vol" && (
				<Field
					label="Ticker"
					value={props.ticker}
					onChange={(e) => props.onTicker(e.target.value.toUpperCase())}
				/>
			)}
		</div>
	);
}
