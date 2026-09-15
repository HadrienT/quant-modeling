import type { DeltaBucketRow } from "@/shared/api/types";
import { NumberCell } from "@/shared/ui";

/**
 * The desk view: a delta-bucketed vol matrix (10/25-delta put, ATM,
 * 25/10-delta call) per maturity, plus risk reversals and butterflies --
 * what an options desk actually reads day to day (dense numbers in delta
 * space), not a strike-indexed surface plot. See VolTab's docstring for
 * the fuller rationale and routers/local_vol_pricing.py's delta_surface
 * for how each cell is derived from the calibrated SVI slice.
 */
export function DeltaMatrix({ rows }: { rows?: DeltaBucketRow[] }) {
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
						</tr>
					))}
				</tbody>
			</table>
		</div>
	);
}
