import { Link } from "@tanstack/react-router";
import type { DeltaBucketRow } from "@/shared/api/types";
import { encodeParams } from "@/shared/products/workbenchLink";
import { NumberCell } from "@/shared/ui";

/**
 * The desk view: a delta-bucketed vol matrix (10/25-delta put, ATM,
 * 25/10-delta call) per maturity, plus risk reversals and butterflies --
 * what an options desk actually reads day to day (dense numbers in delta
 * space), not a strike-indexed surface plot. See VolTab's docstring for
 * the fuller rationale and routers/local_vol_pricing.py's delta_surface
 * for how each cell is derived from the calibrated SVI slice.
 */
export function DeltaMatrix({
	rows,
	spot,
}: {
	rows?: DeltaBucketRow[];
	/** With a spot, each tenor links to the workbench (WP 28, lot U2). */
	spot?: number;
}) {
	if (!rows || rows.length === 0) {
		return <p className="text-sm text-ink-muted">No calibrated slices yet.</p>;
	}

	return (
		<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
			<table className="w-full text-xs">
				<thead>
					<tr className="text-2xs text-ink-muted uppercase">
						{[
							"Tenor",
							"10Δ P",
							"25Δ P",
							"ATM",
							"25Δ C",
							"10Δ C",
							"RR25",
							"BF25",
							"RR10",
							"BF10",
						].map((h) => (
							<th key={h} className="p-2 text-right first:text-left">
								{h}
							</th>
						))}
						{spot != null && <th className="p-2 text-right">Workbench</th>}
					</tr>
				</thead>
				<tbody className="font-mono tabular-nums">
					{rows.map((r) => (
						<tr key={r.ttm} className="border-t border-hairline">
							<td className="p-2 text-left text-ink">{r.tenor_label}</td>
							<td className="p-2 text-right">
								<NumberCell value={r.vol_10p} magnitude="vol" />
							</td>
							<td className="p-2 text-right">
								<NumberCell value={r.vol_25p} magnitude="vol" />
							</td>
							<td className="p-2 text-right text-ink">
								<NumberCell value={r.vol_atm} magnitude="vol" />
							</td>
							<td className="p-2 text-right">
								<NumberCell value={r.vol_25c} magnitude="vol" />
							</td>
							<td className="p-2 text-right">
								<NumberCell value={r.vol_10c} magnitude="vol" />
							</td>
							<td className="p-2 text-right">
								<NumberCell value={r.rr25} magnitude="vol" signed />
							</td>
							<td className="p-2 text-right">
								<NumberCell value={r.bf25} magnitude="vol" signed />
							</td>
							<td className="p-2 text-right text-ink-muted">
								<NumberCell value={r.rr10} magnitude="vol" signed />
							</td>
							<td className="p-2 text-right text-ink-muted">
								<NumberCell value={r.bf10} magnitude="vol" signed />
							</td>
							{spot != null && (
								<td className="p-2 text-right font-sans">
									{r.vol_atm != null && (
										<Link
											to="/price"
											search={{ product: "vanilla", p: atmParams(spot, r) }}
											className="text-accent hover:underline"
											aria-label={`Price an at-the-money ${r.tenor_label} option`}
										>
											Price
										</Link>
									)}
								</td>
							)}
						</tr>
					))}
				</tbody>
			</table>
		</div>
	);
}

/**
 * The workbench form of an at-the-money option on this slice, in the form's
 * units (percent for the vol). Rate and dividend are not in the surface: they
 * stay at the form's defaults, which the caption under the matrix says.
 */
function atmParams(spot: number, r: DeltaBucketRow): string {
	return encodeParams({
		spot: +spot.toFixed(2),
		strike: +spot.toFixed(2),
		maturity: +r.ttm.toFixed(4),
		vol: +(r.vol_atm! * 100).toFixed(2),
	});
}
