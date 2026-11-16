import type { PricingResult } from "@/shared/api";
import { Uncertainty } from "@/shared/ui/density";

type Greeks = PricingResult["greeks"];

/** The three second-order greeks a script can carry, and their units. */
const ROWS: {
	key: "gamma" | "vanna" | "volga";
	name: string;
	unit: string;
}[] = [
	{ key: "gamma", name: "Gamma", unit: "d delta / d spot, per 1.00 of spot" },
	{
		key: "vanna",
		name: "Vanna",
		unit: "d delta / d vol, per 1.00 of vol (÷100 for one vol point)",
	},
	{
		key: "volga",
		name: "Volga",
		unit: "d vega / d vol, per 1.00 of vol (÷100 for one vol point)",
	},
];

/**
 * A script's second-order greeks (blueprint WP 27 §14.10-14.11) as rows of
 * the greeks table, under their own heading, each with its standard error.
 * They are central differences of first-order risks (the adjoint's, or
 * Vibrato's for a payoff that jumps at its final date), so they carry a
 * small bump bias the error does not include; the note below the table says
 * so. Vanna and volga only exist for a model with one vol (Black-Scholes, or
 * a parallel shift of the local-vol grid); a stochastic-vol model has gamma
 * alone.
 */
export function SecondOrderRows({ greeks }: { greeks: Greeks }) {
	const rows = ROWS.filter((r) => greeks[r.key] != null);
	return (
		<tbody aria-label="Second-order greeks">
			<tr>
				<th
					colSpan={3}
					scope="colgroup"
					className="pt-3 pb-1 text-left text-2xs font-normal text-ink-muted uppercase"
				>
					Second order · central differences of first-order risks
				</th>
			</tr>
			{rows.map((r) => (
				<tr key={r.key} className="border-t border-hairline">
					<td className="py-1 pr-2 text-ink">{r.name}</td>
					<td className="px-2 py-1 text-right">
						<Uncertainty
							value={greeks[r.key] ?? 0}
							stdError={greeks[`${r.key}_std_error`] ?? 0}
							magnitude="greek"
						/>
					</td>
					<td className="py-1 pl-2 text-2xs text-ink-secondary">{r.unit}</td>
				</tr>
			))}
		</tbody>
	);
}

/** How the second order was computed, and what its error leaves out. */
export function SecondOrderNote({
	greeks,
	method,
}: {
	greeks: Greeks;
	method: string;
}) {
	const single = greeks.vanna == null && greeks.volga == null;
	return (
		<p className="text-2xs text-ink-muted">
			Second order: {method}. A central difference has a bias of the order of
			the bump squared, which the standard error does not include.
			{single && " This model has no single vol, so no vanna or volga."}
		</p>
	);
}

/** Said where the second-order rows would be, not left in the diagnostics:
 * the user ticked the box and should see why it came back empty. */
export function SecondOrderRefused({ reason }: { reason: string }) {
	return (
		<p
			role="status"
			className="rounded-md border border-warning/40 bg-warning/5 p-3 text-sm text-warning"
		>
			No second-order greeks for this request: {reason}.
		</p>
	);
}
