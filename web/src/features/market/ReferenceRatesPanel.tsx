import { useState } from "react";
import {
	type BenchmarkRate,
	type RateCurrency,
	useRatesHistory,
} from "@/shared/api";
import { Badge, type BadgeProps } from "@/shared/ui";
import { PriceSeriesChart } from "@/shared/viz";

const KIND: Record<
	BenchmarkRate["kind"],
	{ label: string; tone: BadgeProps["tone"]; title: string }
> = {
	overnight: {
		label: "Overnight fixing",
		tone: "accent",
		title: "A one-day rate, published for the date shown.",
	},
	policy: {
		label: "Policy rate",
		tone: "neutral",
		title: "Set by the central bank.",
	},
	compounded: {
		label: "Past average",
		tone: "warning",
		title:
			"Backward-looking: the overnight rate compounded over the PAST period — not a rate for the coming period.",
	},
	interbank_monthly: {
		label: "Monthly average",
		tone: "neutral",
		title:
			"Monthly average of daily fixings (daily fixings are licensed), dated the first of its month.",
	},
};

const YEARS = [1, 5, 10] as const;

/**
 * Reference rates of the currency, as a table with their own dates — fixings
 * and published averages are not points of a term structure, so they are not
 * drawn on a tenor axis — and the history of the overnight benchmark.
 */
export function ReferenceRatesPanel({
	currency,
	benchmarks,
	headline,
}: {
	currency: RateCurrency;
	benchmarks: BenchmarkRate[];
	headline: string;
}) {
	const [years, setYears] = useState<(typeof YEARS)[number]>(5);
	const [selected, setSelected] = useState<string | null>(null);
	const seriesId =
		selected && benchmarks.some((b) => b.series_id === selected)
			? selected
			: headline;
	const history = useRatesHistory(currency, seriesId, years);
	const label = benchmarks.find((b) => b.series_id === seriesId)?.label;

	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">Reference rates</h2>
			<div className="overflow-x-auto rounded-sm border border-hairline">
				<table className="w-full text-sm">
					<thead className="bg-surface-raised text-left text-xs text-ink-secondary">
						<tr>
							<th className="px-3 py-2 font-medium">Rate</th>
							<th className="px-3 py-2 font-medium">Type</th>
							<th className="px-3 py-2 text-right font-medium">Value</th>
							<th className="px-3 py-2 text-right font-medium">As of</th>
							<th className="px-3 py-2" aria-label="History" />
						</tr>
					</thead>
					<tbody>
						{benchmarks.map((b) => {
							const kind = KIND[b.kind];
							return (
								<tr key={b.series_id} className="border-t border-hairline">
									<td className="px-3 py-2 text-ink">{b.label}</td>
									<td className="px-3 py-2">
										<Badge tone={kind.tone} title={kind.title}>
											{kind.label}
										</Badge>
									</td>
									<td className="px-3 py-2 text-right text-ink tabular-nums">
										{b.rate == null ? "—" : `${(b.rate * 100).toFixed(3)}%`}
									</td>
									<td className="px-3 py-2 text-right text-ink-muted tabular-nums">
										{b.as_of ?? "—"}
									</td>
									<td className="px-3 py-2 text-right">
										<button
											type="button"
											onClick={() => setSelected(b.series_id)}
											aria-pressed={b.series_id === seriesId}
											className={
												"text-xs underline decoration-hairline " +
												(b.series_id === seriesId
													? "text-accent"
													: "text-ink-secondary hover:text-ink")
											}
										>
											history
										</button>
									</td>
								</tr>
							);
						})}
					</tbody>
				</table>
			</div>
			<div className="flex items-center justify-end gap-1">
				{YEARS.map((y) => (
					<button
						key={y}
						type="button"
						aria-pressed={y === years}
						onClick={() => setYears(y)}
						className={
							"rounded-sm border px-2 py-0.5 text-xs " +
							(y === years
								? "border-accent text-ink"
								: "border-hairline text-ink-secondary")
						}
					>
						{y}Y
					</button>
				))}
			</div>
			<PriceSeriesChart
				title={`${label ?? seriesId} — history (% per annum)`}
				isLoading={history.isLoading}
				error={history.error}
				height={240}
				candles={history.data?.points.map((p) => {
					const v = p.rate * 100;
					return { time: p.date, open: v, high: v, low: v, close: v };
				})}
			/>
		</section>
	);
}
