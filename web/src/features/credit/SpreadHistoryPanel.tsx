import { useState } from "react";
import { type CreditSpreadsResponse, useSpreadHistory } from "@/shared/api";
import { Segmented } from "@/shared/ui";
import { PriceSeriesChart } from "@/shared/viz";

const YEARS = [1, 3, 10, 40] as const;

/**
 * History of one series: a rating or maturity bucket's OAS (bp; three years
 * at most, ICE's limit on FRED), or a Moody's yield (%; decades).
 */
export function SpreadHistoryPanel({ data }: { data: CreditSpreadsResponse }) {
	const options = [
		...data.aggregates,
		...data.ratings,
		...data.term_structure.map((q) => ({ ...q, label: `IG ${q.label}` })),
		...data.reference_yields,
	].map((s) => ({ value: s.series_id, label: s.label }));
	const [seriesId, setSeriesId] = useState(options[0]?.value ?? "BAMLC0A0CM");
	const [years, setYears] = useState<(typeof YEARS)[number]>(3);
	const history = useSpreadHistory(seriesId, years);
	const isYield = history.data?.kind === "yield";
	const scale = isYield ? 100 : 1e4;

	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">History</h2>
			<div className="flex flex-wrap items-center justify-between gap-2">
				<select
					aria-label="Series"
					value={seriesId}
					onChange={(e) => setSeriesId(e.target.value)}
					className="rounded-sm border border-hairline bg-surface px-2 py-1 text-xs text-ink"
				>
					{options.map((o) => (
						<option key={o.value} value={o.value}>
							{o.label}
						</option>
					))}
				</select>
				<Segmented
					label="Window"
					options={YEARS.map((y) => ({ value: y, label: `${y}Y` }))}
					value={years}
					onChange={setYears}
				/>
			</div>
			<PriceSeriesChart
				title={`${history.data?.label ?? seriesId} — ${isYield ? "% per annum" : "bp"}`}
				isLoading={history.isLoading}
				error={history.error}
				height={240}
				candles={history.data?.points.map((p) => {
					const v = p.value * scale;
					return { time: p.date, open: v, high: v, low: v, close: v };
				})}
			/>
		</section>
	);
}
