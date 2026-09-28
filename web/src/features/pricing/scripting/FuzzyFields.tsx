import { Field } from "@/shared/ui";

/** Fuzzy logic for a script: smoothed comparisons, and their default width. */
export function FuzzyFields(props: {
	fuzzy: boolean;
	onFuzzy: (v: boolean) => void;
	eps: string;
	onEps: (v: string) => void;
}) {
	return (
		<div className="flex flex-wrap items-center gap-4">
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
