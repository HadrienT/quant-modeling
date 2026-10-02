import { type RatesCurveQuotes, useRatesCurves } from "@/shared/api";
import { tenorLabel } from "@/shared/format";
import { Methodology } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { RatesCurveChart } from "@/shared/viz";

/**
 * The curves the price rests on, built from the quotes: OIS zero and forward
 * rates, and the index forwards bootstrapped on OIS discounting. With a term
 * index the gap between the two forward curves is the basis a single curve
 * could not hold; with an overnight index there is one curve and no gap.
 */
export function CurvesPanel({ quotes }: { quotes: RatesCurveQuotes }) {
	const q = useRatesCurves(quotes);
	if (q.error)
		return <ErrorState error={q.error} onRetry={() => q.refetch()} />;
	if (!q.data) return <ChartSkeleton />;
	const curves = q.data.curves;
	const p = curves.points;
	const period = tenorLabel(1 / (quotes.float_frequency ?? 2));
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">
				{curves.single_curve
					? "Discount and projection curve"
					: "Discount and projection curves"}
			</h2>
			<RatesCurveChart
				title="Rates (per annum)"
				height={260}
				series={[
					{
						label: "OIS zero (continuous)",
						points: p.map((x) => ({ x: x.tenor, y: x.ois_zero })),
					},
					{
						label: `OIS ${period} forward`,
						points: p.map((x) => ({ x: x.tenor, y: x.ois_forward })),
					},
					...(curves.single_curve
						? []
						: [
								{
									label: `Index ${period} forward`,
									points: p.map((x) => ({ x: x.tenor, y: x.index_forward })),
								},
							]),
				]}
			/>
			{curves.single_curve ? (
				<p className="text-xs text-ink-secondary">
					One curve: the floating index is the overnight rate itself, so the
					curve that discounts also projects, and the basis is zero. The
					illustrative EUR set shows the two-curve case.
				</p>
			) : (
				<RatesCurveChart
					title="Index − OIS forward basis"
					height={220}
					series={[
						{
							label: `${period} basis`,
							points: p.map((x) => ({ x: x.tenor, y: x.basis_bp / 1e4 })),
						},
					]}
				/>
			)}
			<p className="text-xs text-ink-muted">
				OIS pillars at {curves.ois_pillars.map(tenorLabel).join(", ")}; index
				pillars at {curves.index_pillars.map(tenorLabel).join(", ")}. Every
				input swap reprices within{" "}
				{curves.max_repricing_error_bp.toExponential(1)} bp of its quote.
			</p>
			<Methodology
				id="rates-curves-methodology"
				title="How the curves are built"
				sections={q.data.methodology}
			/>
		</section>
	);
}
