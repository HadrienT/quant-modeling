import type { PricingResult } from "@/shared/api";
import { Badge } from "@/shared/ui";
import { Uncertainty } from "@/shared/ui/density";

type Risk = NonNullable<PricingResult["risks"]>[number];

/** What each model parameter's sensitivity is called, and its unit. */
const NAMES: Record<string, [name: string, unit: string]> = {
	spot: ["Delta", "per 1.00 of spot"],
	vol: ["Vega", "per 1.00 of vol (÷100 for one vol point)"],
	rate: ["Rho", "per 1.00 of rate (÷10,000 for one bp)"],
	div: ["Dividend yield", "per 1.00 of yield (÷10,000 for one bp)"],
	v0: ["Heston v₀", "initial variance"],
	kappa: ["Heston κ", "mean reversion of the variance"],
	theta: ["Heston θ", "long-run variance"],
	xi: ["Heston ξ", "vol of vol"],
	rho: ["Spot / vol correlation ρ", "model parameter, not the rate greek"],
	eta: ["Rough Bergomi η", "vol of vol"],
	xi0: ["Rough Bergomi ξ₀", "flat forward variance"],
};

/** A local-vol or leverage grid node: one sensitivity per (strike, date). */
const GRID = /^(lvol|leverage)\[\d+,\d+\]$/;

function describe(label: string): [string, string] {
	const indexed = /^(\w+)\[(\d+)\]$/.exec(label);
	const [name, unit] = NAMES[indexed ? indexed[1]! : label] ?? [label, ""];
	return indexed
		? [`${name} · underlying ${Number(indexed[2]) + 1}`, unit]
		: [name, unit];
}

/**
 * The sensitivities of a scripted price to every parameter of its model,
 * from one adjoint Monte-Carlo run (blueprint WP 17), each with its standard
 * error. A value within two standard errors of zero is marked: it is noise,
 * not a greek. Parameters whose sensitivity is exactly zero are named apart
 * rather than tabulated as "0".
 */
export function ScriptGreeks({ risks }: { risks: Risk[] }) {
	const grid = risks.filter((r) => GRID.test(r.label));
	const scalars = risks.filter((r) => !GRID.test(r.label));
	const zero = scalars.filter((r) => r.value === 0 && r.std_error === 0);
	const rows = scalars.filter((r) => !zero.includes(r));
	return (
		<section
			aria-label="Greeks"
			className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-4"
		>
			<h2 className="text-2xs text-ink-muted uppercase">
				Greeks · adjoint, same paths as the price
			</h2>
			<table className="w-full text-sm">
				<thead>
					<tr className="text-left text-2xs text-ink-muted uppercase">
						<th className="py-1 pr-2 font-normal">Sensitivity to</th>
						<th className="px-2 py-1 text-right font-normal">
							Value ± std error
						</th>
						<th className="py-1 pl-2 font-normal">Unit</th>
					</tr>
				</thead>
				<tbody>
					{rows.map((r) => {
						const [name, unit] = describe(r.label);
						const noise = Math.abs(r.value) <= 2 * r.std_error;
						return (
							<tr key={r.label} className="border-t border-hairline">
								<td className="py-1 pr-2 text-ink">
									{name}
									{noise && (
										<Badge tone="neutral" className="ml-2">
											not significant
										</Badge>
									)}
								</td>
								<td className="px-2 py-1 text-right">
									<Uncertainty
										value={r.value}
										stdError={r.std_error}
										magnitude="greek"
									/>
								</td>
								<td className="py-1 pl-2 text-2xs text-ink-secondary">
									{unit}
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>
			{zero.length > 0 && (
				<p className="text-2xs text-ink-muted">
					Exactly zero, so not tabulated:{" "}
					{zero.map((r) => describe(r.label)[0]).join(", ")}. Either the
					parameter does not enter this price, or the payoff jumps and nothing
					smooths it (see the warning, if any).
				</p>
			)}
			{grid.length > 0 && (
				<p className="text-2xs text-ink-muted">
					Plus {grid.length} sensitivities to the nodes of the model&apos;s
					volatility grid, not listed here: the model has no single vol to take
					a vega to.
				</p>
			)}
		</section>
	);
}
