import type { CreditSpreadsResponse } from "@/shared/api";
import { Badge } from "@/shared/ui";
import { RatesCurveChart, type RateSeries } from "@/shared/viz";
import { pct } from "./format";

/** λ is constant on (t_{i-1}, t_i]: drawn as steps, not joined by slopes. */
function hazardSteps(times: number[], hazards: number[]) {
	return times.flatMap((t, i) => [
		{ x: i === 0 ? 0 : times[i - 1]!, y: hazards[i]! },
		{ x: t, y: hazards[i]! },
	]);
}

/**
 * The only free credit term structure — US investment grade by maturity
 * bucket — as quoted dots, the hazard rate bootstrapped from it as steps, and
 * the cumulative risk-neutral default probability that hazard implies.
 */
export function TermStructurePanel({ data }: { data: CreditSpreadsResponse }) {
	const quoted: RateSeries = {
		label: "OAS by maturity bucket (quoted)",
		line: false,
		points: data.term_structure.map((q) => ({ x: q.tenor, y: q.spread })),
	};
	const h = data.hazard;
	const curve: RateSeries[] = [quoted];
	if (h)
		curve.push({
			label: "Hazard rate λ(t) (bootstrapped)",
			points: hazardSteps(h.times, h.hazards),
		});

	return (
		<section className="flex flex-col gap-3">
			<div className="flex flex-wrap items-baseline justify-between gap-2">
				<h2 className="text-sm font-semibold text-ink">
					US investment grade — spread and hazard term structure
				</h2>
				<div className="flex items-center gap-2 text-xs text-ink-muted">
					<Badge>ICE BofA OAS · FRED</Badge>
					<span>as of {data.as_of}</span>
					<span>· discounting: UST curve of {data.discount_curve_as_of}</span>
				</div>
			</div>
			<div className="grid gap-4 lg:grid-cols-2">
				<RatesCurveChart
					title="Spread and hazard rate (per annum)"
					series={curve}
					height={280}
				/>
				{h ? (
					<RatesCurveChart
						title="Cumulative default probability (risk-neutral)"
						series={[
							{
								label: `1 − S(t), recovery ${pct(data.recovery, 0)}`,
								points: h.grid.map((t, i) => ({
									x: t,
									y: 1 - h.survival[i]!,
								})),
							},
						]}
						height={280}
					/>
				) : (
					<p className="rounded-sm border border-hairline bg-surface p-3 text-sm text-ink-secondary">
						{data.hazard_unavailable}
					</p>
				)}
			</div>
			{h && (
				<p className="text-xs text-ink-muted">
					Dots are the published bucket spreads, placed at bucket midpoints
					(15Y+ at 20 years, a convention). The steps are the piecewise-constant
					hazard rate that prices a par CDS at each bucket&apos;s spread; the
					largest repricing error of the inputs is{" "}
					{h.max_repricing_error_bp.toExponential(1)} bp.
				</p>
			)}
		</section>
	);
}
