import type { GovernmentCurveResponse } from "@/shared/api";
import { Badge } from "@/shared/ui";
import { RatesCurveChart, type RateSeries } from "@/shared/viz";

const QUOTE_LABEL: Record<GovernmentCurveResponse["quote"], string> = {
	par_semiannual: "Par yields (semi-annual)",
	zero_continuous: "Zero rates (continuous)",
};

const tenorLabel = (years: number) =>
	years < 1 ? `${Math.round(years * 12)}M` : `${years}Y`;

/**
 * The government curve: the published quotes as dots (they are the data), and
 * — when derivable — the bootstrapped zero curve and the forward curve as
 * lines. A currency without a free curve says why instead of drawing a proxy.
 */
export function GovernmentCurvePanel({
	curve,
	unavailable,
}: {
	curve: GovernmentCurveResponse | null | undefined;
	unavailable: string | null | undefined;
}) {
	if (!curve)
		return (
			<section className="flex flex-col gap-2">
				<h2 className="text-sm font-semibold text-ink">Government curve</h2>
				<p className="rounded-sm border border-hairline bg-surface p-3 text-sm text-ink-secondary">
					{unavailable ?? "No government curve for this currency."}
				</p>
			</section>
		);

	const quoted: RateSeries = {
		label: `Quoted — ${QUOTE_LABEL[curve.quote].toLowerCase()}`,
		line: false,
		points: curve.quoted.map((q) => ({ x: q.tenor, y: q.rate })),
	};
	const series: RateSeries[] = [quoted];
	if (curve.zero)
		series.push({
			label:
				curve.quote === "zero_continuous"
					? "Zero (published, interpolated)"
					: "Zero (bootstrapped, continuous)",
			points: curve.zero.map((p) => ({ x: p.tenor, y: p.rate })),
		});
	if (curve.forward)
		series.push({
			label: `Forward ${tenorLabel(curve.forward_period_years)} (continuous)`,
			points: curve.forward.map((p) => ({ x: p.tenor, y: p.rate })),
		});

	return (
		<section className="flex flex-col gap-2">
			<div className="flex flex-wrap items-baseline justify-between gap-2">
				<h2 className="text-sm font-semibold text-ink">{curve.name}</h2>
				<div className="flex items-center gap-2 text-xs text-ink-muted">
					<Badge>{QUOTE_LABEL[curve.quote]}</Badge>
					<span>as of {curve.as_of}</span>
					<a
						href={curve.source_url}
						target="_blank"
						rel="noreferrer"
						className="underline decoration-hairline hover:text-ink"
					>
						{curve.source}
					</a>
				</div>
			</div>
			<RatesCurveChart title="Term structure" series={series} height={300} />
			{curve.zero && (
				<p className="text-xs text-ink-muted">
					Dots are the published quotes. Lines are derived: sampled between the
					first and last quote with the discount curve&apos;s own interpolation
					(log-linear discount factors), never extrapolated.
				</p>
			)}
			{curve.no_derivation && (
				<p className="text-xs text-ink-muted">{curve.no_derivation}</p>
			)}
		</section>
	);
}
