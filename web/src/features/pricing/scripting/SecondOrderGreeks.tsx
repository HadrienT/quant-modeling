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
 * A script's second-order greeks (blueprint WP 27 §14.10), each with its
 * standard error, and the method that gave them, said in full: they are
 * central differences of the adjoint's first-order risks, so they carry a
 * small bump bias the error does not include. Vanna and volga only exist for
 * a model with one vol (Black-Scholes, or a parallel shift of the local-vol
 * grid); a stochastic-vol model shows gamma alone.
 */
export function SecondOrderGreeks({
	greeks,
	method,
}: {
	greeks: Greeks;
	method: string;
}) {
	const rows = ROWS.filter((r) => greeks[r.key] != null);
	return (
		<section
			aria-label="Second-order greeks"
			className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-4"
		>
			<h2 className="text-2xs text-ink-muted uppercase">
				Second-order greeks · bump on the adjoint
			</h2>
			<table className="w-full text-sm">
				<thead>
					<tr className="text-left text-2xs text-ink-muted uppercase">
						<th className="py-1 pr-2 font-normal">Greek</th>
						<th className="px-2 py-1 text-right font-normal">
							Value ± std error
						</th>
						<th className="py-1 pl-2 font-normal">Unit</th>
					</tr>
				</thead>
				<tbody>
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
							<td className="py-1 pl-2 text-2xs text-ink-secondary">
								{r.unit}
							</td>
						</tr>
					))}
				</tbody>
			</table>
			<p className="text-2xs text-ink-muted">
				{method}. A central difference has a bias of the order of the bump
				squared, which the standard error does not include.
				{rows.length === 1 &&
					" This model has no single vol, so no vanna or volga."}
			</p>
		</section>
	);
}
