import type { RatesAnalysisResponse } from "@/shared/api";
import { Metric, MetricRow } from "@/shared/ui";
import { bp, pct, tenorLabel } from "./format";

/**
 * Hull-White calibrated to the ATM swaption grid: the parameters, the fit in
 * bp of normal vol, and quote by quote the market and model vols.
 */
export function CalibrationPanel({
	hw,
}: {
	hw: RatesAnalysisResponse["hull_white"];
}) {
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">Hull-White calibration</h2>
			<MetricRow>
				<Metric
					label="Mean reversion a"
					value={hw.mean_reversion.toFixed(4)}
					footnote={hw.mean_reversion_fixed ? "Fixed by you" : "Fitted"}
				/>
				<Metric label="Short-rate vol σ" value={bp(hw.sigma)} />
				<Metric
					label="RMSE"
					value={`${hw.rmse_bp.toFixed(2)} bp`}
					footnote="Of normal vol"
				/>
				<Metric label="Worst quote" value={`${hw.worst_bp.toFixed(2)} bp`} />
				<Metric
					label="Solver"
					value={hw.converged ? "Converged" : "Not converged"}
					footnote={`${hw.iterations} iterations, ${(hw.seconds * 1000).toFixed(0)} ms`}
				/>
			</MetricRow>
			<div className="overflow-x-auto">
				<table className="w-full text-sm">
					<caption className="sr-only">Hull-White fit by swaption</caption>
					<thead>
						<tr className="text-left text-xs text-ink-muted">
							<th className="px-2 py-1 font-normal">Swaption</th>
							<th className="px-2 py-1 text-right font-normal">ATM strike</th>
							<th className="px-2 py-1 text-right font-normal">Market vol</th>
							<th className="px-2 py-1 text-right font-normal">Model vol</th>
							<th className="px-2 py-1 text-right font-normal">
								Model − market
							</th>
						</tr>
					</thead>
					<tbody className="font-mono tabular-nums">
						{hw.points.map((p) => (
							<tr
								key={`${p.expiry}-${p.tenor}`}
								className="border-t border-hairline"
							>
								<td className="px-2 py-1 font-sans">
									{tenorLabel(p.expiry)} into {tenorLabel(p.tenor)}
								</td>
								<td className="px-2 py-1 text-right">{pct(p.strike)}</td>
								<td className="px-2 py-1 text-right">{bp(p.market_vol)}</td>
								<td className="px-2 py-1 text-right">{bp(p.model_vol)}</td>
								<td className="px-2 py-1 text-right">
									{p.model_vol == null
										? "—"
										: bp(p.model_vol - p.market_vol, 2)}
								</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>
		</section>
	);
}
