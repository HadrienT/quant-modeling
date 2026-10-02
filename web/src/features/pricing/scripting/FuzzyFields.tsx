import { Field } from "@/shared/ui";

/**
 * Greeks and fuzzy logic for a script. The greeks come from one adjoint run
 * on the price's own paths; smoothed comparisons (and their default width)
 * are what gives a digital or a barrier greeks at all.
 */
export function FuzzyFields(props: {
	greeks: boolean;
	onGreeks: (v: boolean) => void;
	fuzzy: boolean;
	onFuzzy: (v: boolean) => void;
	eps: string;
	onEps: (v: string) => void;
}) {
	return (
		<div className="flex flex-wrap items-center gap-4">
			<label
				className="flex items-center gap-2 text-sm"
				title="Every model parameter's sensitivity from one adjoint Monte-Carlo run, at about 3 to 5 times the cost of the price alone."
			>
				<input
					type="checkbox"
					checked={props.greeks}
					onChange={(e) => props.onGreeks(e.target.checked)}
				/>
				Greeks (adjoint)
			</label>
			<label className="flex items-center gap-2 text-sm">
				<input
					type="checkbox"
					checked={props.fuzzy}
					onChange={(e) => props.onFuzzy(e.target.checked)}
				/>
				Fuzzy (smoothed comparisons, for digitals and barriers)
			</label>
			{props.fuzzy && (
				<Field
					label="Default eps"
					inputMode="decimal"
					value={props.eps}
					onChange={(e) => props.onEps(e.target.value)}
				/>
			)}
		</div>
	);
}
