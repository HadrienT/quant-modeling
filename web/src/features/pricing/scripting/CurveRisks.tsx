import type { PricingResult } from "@/shared/api";
import { Uncertainty } from "@/shared/ui/density";

type Risk = NonNullable<PricingResult["risks"]>[number];

/** A pillar of the market's forward curve (blueprint WP 32): the risk to its
 * zero rate, or to its dividend yield, by its time in years. */
export const PILLAR = /^(zero|div_yield)\[([\d.eE+-]+)\]$/;

type Row = { t: number; zero?: Risk; div?: Risk };

function rows(pillars: Risk[]): Row[] {
	const by = new Map<number, Row>();
	for (const r of pillars) {
		const m = PILLAR.exec(r.label)!;
		const t = Number(m[2]);
		const row = by.get(t) ?? { t };
		if (m[1] === "zero") row.zero = r;
		else row.div = r;
		by.set(t, row);
	}
	return [...by.values()].sort((a, b) => a.t - b.t);
}

function tenor(t: number): string {
	const days = Math.round(t * 365);
	return days < 60 ? `${days} d` : `${t.toFixed(2)} y`;
}

function Cell({ risk }: { risk?: Risk }) {
	if (!risk || (risk.value === 0 && risk.std_error === 0))
		return <span className="text-ink-muted">0</span>;
	return (
		<Uncertainty
			value={risk.value}
			stdError={risk.std_error}
			magnitude="greek"
		/>
	);
}

/**
 * Rho and the dividend risk split by expiry: one adjoint risk per pillar of
 * the forward curve the price was simulated on, summing to the Rho and the
 * dividend yield above (the parallel shift). Only the expiries the product
 * reads are listed; the others are exactly zero.
 */
export function CurveRisks({ pillars }: { pillars: Risk[] }) {
	const all = rows(pillars);
	const read = all.filter(
		(r) => (r.zero && r.zero.value !== 0) || (r.div && r.div.value !== 0),
	);
	if (all.length === 0) return null;
	return (
		<details className="text-sm">
			<summary className="cursor-pointer text-2xs text-ink-muted uppercase select-none">
				Rho and dividend by expiry · {read.length} of {all.length} expiries
			</summary>
			<table className="mt-2 w-full">
				<thead>
					<tr className="text-left text-2xs text-ink-muted uppercase">
						<th className="py-1 pr-2 font-normal">Expiry</th>
						<th className="px-2 py-1 text-right font-normal">Zero rate</th>
						<th className="px-2 py-1 text-right font-normal">Dividend yield</th>
					</tr>
				</thead>
				<tbody>
					{read.map((r) => (
						<tr key={r.t} className="border-t border-hairline">
							<td className="py-1 pr-2 text-ink">{tenor(r.t)}</td>
							<td className="px-2 py-1 text-right">
								<Cell risk={r.zero} />
							</td>
							<td className="px-2 py-1 text-right">
								<Cell risk={r.div} />
							</td>
						</tr>
					))}
				</tbody>
			</table>
			<p className="mt-1 text-2xs text-ink-muted">
				Per 1.00 of the expiry&apos;s zero rate or dividend yield (÷10,000 for
				one bp). A zero rate moves that expiry&apos;s discount and forward
				together, as Rho does; the rows sum to Rho and to the dividend yield
				risk above.
			</p>
		</details>
	);
}
