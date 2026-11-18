import type { PricingResult } from "@/shared/api";
import { Badge } from "@/shared/ui";
import { Uncertainty } from "@/shared/ui/density";
import { SecondOrderNote, SecondOrderRows } from "./SecondOrderGreeks";

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
	// Hull-White rates (a rates product of the library)
	rate_vol: ["Hull-White σ", "per 1.00 of short-rate vol (÷10,000 for one bp)"],
	curve_parallel: [
		"Curve, every quote together",
		"per 1.00 of rate (÷10,000 for the DV01)",
	],
};

/** A curve factor: a par swap rate the curve was built from, or a pillar's
 * zero rate, by its tenor in years. */
const CURVE = /^(swap_rate|zero)\[([\d.]+)\]$/;

/** A local-vol or leverage grid node: one sensitivity per (strike, date),
 * `lvol[a:i,j]` for underlying a of several. */
const GRID = /^(lvol|leverage)\[(?:(\d+):)?\d+,\d+\]$/;

function describe(label: string, names: string[] = []): [string, string] {
	const curve = CURVE.exec(label);
	if (curve)
		return [
			curve[1] === "swap_rate"
				? `SOFR ${curve[2]}Y par swap rate`
				: `Zero rate at ${curve[2]}Y`,
			"per 1.00 of rate (÷10,000 for one bp)",
		];
	const indexed = /^(\w+)\[(\d+)\]$/.exec(label);
	const [name, unit] = NAMES[indexed ? indexed[1]! : label] ?? [label, ""];
	const i = indexed ? Number(indexed[2]) : -1;
	return indexed
		? [`${name} · ${names[i] ?? `underlying ${i + 1}`}`, unit]
		: [name, unit];
}

/** The grid nodes' risks summed per surface (per underlying): the price's
 * move per unit of vol when the whole surface shifts in parallel. */
function surfaces(grid: Risk[], names: string[]) {
	const by = new Map<string, { value: number; nodes: number; kind: string }>();
	for (const r of grid) {
		const m = GRID.exec(r.label)!;
		const key = m[2] ?? "0";
		const cur = by.get(key) ?? { value: 0, nodes: 0, kind: m[1]! };
		cur.value += r.value;
		cur.nodes += 1;
		by.set(key, cur);
	}
	return [...by.entries()].map(([k, v]) => ({
		name: `${v.kind === "lvol" ? "Local-vol surface" : "Leverage"}${
			by.size > 1 || names.length > 1
				? ` · ${names[Number(k)] ?? `underlying ${Number(k) + 1}`}`
				: ""
		}`,
		...v,
	}));
}

/**
 * The sensitivities of a scripted price to every parameter of its model,
 * from one adjoint Monte-Carlo run (blueprint WP 17), each with its standard
 * error. A value within two standard errors of zero is marked: it is noise,
 * not a greek. Parameters whose sensitivity is exactly zero are named apart
 * rather than tabulated as "0".
 */
export function ScriptGreeks({
	risks,
	zeroDeltaNote,
	secondOrder,
	names = [],
}: {
	risks: Risk[];
	/** the underlyings' tickers, spot(0) first, to name their greeks */
	names?: string[];
	/** why a delta can be exactly zero here, said when it is */
	zeroDeltaNote?: string;
	/** gamma, vanna and volga when the request asked for them, and how */
	secondOrder?: { greeks: PricingResult["greeks"]; method: string };
}) {
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
						const [name, unit] = describe(r.label, names);
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
					{surfaces(grid, names).map((s) => (
						<tr key={s.name} className="border-t border-hairline">
							<td className="py-1 pr-2 text-ink">
								{s.name}, parallel shift
								<Badge tone="neutral" className="ml-2">
									{s.nodes} nodes
								</Badge>
							</td>
							<td className="px-2 py-1 text-right">
								<Uncertainty
									value={s.value}
									stdError={null}
									magnitude="greek"
								/>
							</td>
							<td className="py-1 pl-2 text-2xs text-ink-secondary">
								per 1.00 of vol on every node (÷100 for one vol point); the sum
								of the nodes&apos; adjoint risks
							</td>
						</tr>
					))}
				</tbody>
				{secondOrder && <SecondOrderRows greeks={secondOrder.greeks} />}
			</table>
			{secondOrder && <SecondOrderNote {...secondOrder} />}
			{zero.length > 0 && (
				<p className="text-2xs text-ink-muted">
					Exactly zero, so not tabulated:{" "}
					{zero.map((r) => describe(r.label, names)[0]).join(", ")}. Either the
					parameter does not enter this price, or the payoff jumps and nothing
					smooths it (see the warning, if any).
					{zeroDeltaNote &&
						zero.some((r) => /^spot(\[\d+\])?$/.test(r.label)) &&
						` ${zeroDeltaNote}`}
				</p>
			)}
			{grid.length > 0 && (
				<p className="text-2xs text-ink-muted">
					The model has no single vol to take a vega to: its {grid.length} grid
					nodes each have a risk, summed above as the vega to a parallel shift
					of the surface (its error is not shown: the nodes&apos; errors are
					correlated).
				</p>
			)}
		</section>
	);
}
