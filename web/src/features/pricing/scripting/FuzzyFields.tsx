import { Field } from "@/shared/ui";

/** No greeks, the adjoint's first order, or that plus gamma, vanna, volga. */
export type GreeksChoice = "none" | "first" | "second";

/**
 * Greeks and fuzzy logic for a script. The greeks come from one adjoint run
 * on the price's own paths; the second order from that adjoint bumped (on
 * the GPU); smoothed comparisons (and their default width) are what gives a
 * digital or a barrier greeks at all.
 */
export function FuzzyFields(props: {
	greeks: GreeksChoice;
	onGreeks: (v: GreeksChoice) => void;
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
					checked={props.greeks !== "none"}
					onChange={(e) => props.onGreeks(e.target.checked ? "first" : "none")}
				/>
				Greeks (adjoint)
			</label>
			<label
				className="flex items-center gap-2 text-sm"
				title="Gamma, vanna and volga with their errors: the adjoint's risks bumped (spot 1 %, vol 25 bp) on the same Sobol points. On the GPU only, about four adjoint runs more."
			>
				<input
					type="checkbox"
					disabled={props.greeks === "none"}
					checked={props.greeks === "second"}
					onChange={(e) =>
						props.onGreeks(e.target.checked ? "second" : "first")
					}
				/>
				+ second order (gamma, vanna, volga · GPU)
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
