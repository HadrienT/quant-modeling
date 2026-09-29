import type { RatesAnalysisResponse } from "@/shared/api";
import { RatesCurveChart } from "@/shared/viz";
import { tenorLabel } from "./format";

/**
 * The two curves the analysis built: OIS zero and forward rates, and the
 * index forwards bootstrapped on OIS discounting — the gap between the two
 * forward curves is the basis a single curve could not hold.
 */
export function CurvesPanel({
	curves,
	indexPeriod,
}: {
	curves: RatesAnalysisResponse["curves"];
	indexPeriod: number;
}) {
	const p = curves.points;
	const period = tenorLabel(indexPeriod);
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">
				Discount and projection curves
			</h2>
			<div className="grid gap-4 lg:grid-cols-2">
				<RatesCurveChart
					title="Rates (per annum)"
					height={280}
					series={[
						{
							label: "OIS zero (continuous)",
							points: p.map((q) => ({ x: q.tenor, y: q.ois_zero })),
						},
						{
							label: `OIS ${period} forward`,
							points: p.map((q) => ({ x: q.tenor, y: q.ois_forward })),
						},
						{
							label: `Index ${period} forward`,
							points: p.map((q) => ({ x: q.tenor, y: q.index_forward })),
						},
					]}
				/>
				<RatesCurveChart
					title="Index − OIS forward basis"
					height={280}
					series={[
						{
							label: `${period} basis`,
							points: p.map((q) => ({ x: q.tenor, y: q.basis_bp / 1e4 })),
						},
					]}
				/>
			</div>
			<p className="text-xs text-ink-muted">
				OIS pillars at {curves.ois_pillars.map(tenorLabel).join(", ")}; index
				pillars at {curves.index_pillars.map(tenorLabel).join(", ")}. Every
				input swap reprices within{" "}
				{curves.max_repricing_error_bp.toExponential(1)} bp of its quote.
			</p>
		</section>
	);
}
