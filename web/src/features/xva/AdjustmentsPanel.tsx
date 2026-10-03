import type { XvaAdjustments, XvaResponse } from "@/shared/api";
import { Explained } from "./Explained";
import { plusMinus, signed } from "./format";
import { ADJUSTMENTS, type AdjustmentKey, GREGORY } from "./texts";

/** The value and, where it is a Monte-Carlo estimate, the error of one row. */
function figures(a: XvaAdjustments, key: AdjustmentKey) {
	switch (key) {
		case "cva":
			return { value: a.cva.value, error: a.cva.error };
		case "dva":
			return { value: a.dva.value, error: a.dva.error };
		case "fva":
			return { value: a.fca + a.fba };
		case "colva":
			return { value: a.colva };
		case "mva":
			return { value: a.mva };
		case "kva":
			return { value: a.kva };
	}
}

const total = (a: XvaAdjustments) =>
	ADJUSTMENTS.reduce((sum, r) => sum + figures(a, r.key).value, 0);

/**
 * The adjustments, one row each, signed as cash flows (a cost is negative),
 * with the same netting set without its collateral next to it when there is
 * a CSA — then each one explained: what it is, its formula, its source.
 */
export function AdjustmentsPanel({ data }: { data: XvaResponse }) {
	const a = data.adjustments;
	const open = data.adjustments_uncollateralised;
	const wrongWay = a.cva.value !== a.cva_independent.value;

	return (
		<div className="flex flex-col gap-4">
			<div className="overflow-x-auto rounded-sm border border-hairline">
				<table className="w-full text-sm">
					<thead className="bg-surface-raised text-left text-xs text-ink-secondary">
						<tr>
							<th className="px-3 py-2 font-medium">Adjustment</th>
							<th className="px-3 py-2 text-right font-medium">
								{open ? "Under the CSA" : "Value"}
							</th>
							{open && (
								<th className="px-3 py-2 text-right font-medium">
									Without collateral
								</th>
							)}
						</tr>
					</thead>
					<tbody>
						{ADJUSTMENTS.map((row) => {
							const f = figures(a, row.key);
							return (
								<tr key={row.key} className="border-t border-hairline">
									<td className="px-3 py-2 text-ink">
										{row.label}
										<span className="ml-2 text-xs text-ink-muted">
											{row.name}
										</span>
									</td>
									<td className="px-3 py-2 text-right tabular-nums">
										{signed(f.value)}{" "}
										<span className="text-xs text-ink-muted">
											{plusMinus(f.error)}
										</span>
									</td>
									{open && (
										<td className="px-3 py-2 text-right text-ink-secondary tabular-nums">
											{signed(figures(open, row.key).value)}
										</td>
									)}
								</tr>
							);
						})}
						<tr className="border-t border-hairline bg-surface-raised font-medium">
							<td className="px-3 py-2 text-ink">Total adjustment</td>
							<td className="px-3 py-2 text-right tabular-nums">
								{signed(total(a))}
							</td>
							{open && (
								<td className="px-3 py-2 text-right tabular-nums">
									{signed(total(open))}
								</td>
							)}
						</tr>
					</tbody>
				</table>
			</div>
			<p className="text-xs text-ink-muted">
				Cash-flow convention: a cost to the bank is negative. ± is the
				Monte-Carlo standard error of the figure, estimated path by path for
				the CVA and the DVA; the other adjustments are integrals of expected
				profiles, whose error the engine does not estimate yet. Rule of thumb
				for the CVA,
				credit spread × average exposure × maturity:{" "}
				{signed(a.cva_rule_of_thumb)}.
				{wrongWay &&
					` With the counterparty's default independent of the exposure the CVA would be ${signed(a.cva_independent.value)}.`}
			</p>
			{ADJUSTMENTS.map((row) => (
				<Explained
					key={row.key}
					title={`${row.label} — ${row.name}`}
					tex={row.tex}
					source={row.source}
					figures={
						<span className="text-sm text-ink tabular-nums">
							{signed(figures(a, row.key).value)}
						</span>
					}
				>
					{row.text}
				</Explained>
			))}
			<p className="text-2xs text-ink-muted">
				EE* and ENE* are the discounted expected exposures. Notation and signs:{" "}
				{GREGORY}.
			</p>
		</div>
	);
}
