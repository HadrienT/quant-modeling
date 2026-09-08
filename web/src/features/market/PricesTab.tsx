import { useMemo, useState } from "react";
import { useTickers, usePriceHistory } from "@/shared/api";
import { Combobox } from "@/shared/ui";
import {
	DeltaBadge,
	Freshness,
	Metric,
	MetricRow,
	NumberCell,
} from "@/shared/ui";
import { PriceSeriesChart } from "@/shared/viz";
import { realizedVol } from "./realizedVol";

const RANGES = ["1M", "3M", "YTD", "1Y", "5Y", "max"];

export function PricesTab({
	ticker,
	onTicker,
}: {
	ticker: string;
	onTicker: (t: string) => void;
}) {
	const tickers = useTickers();
	const [range, setRange] = useState("1Y");
	const history = usePriceHistory(ticker || null, range);

	const stats = useMemo(() => {
		const pts = history.data?.points ?? [];
		if (pts.length < 2) return null;
		const closes = pts.map((p) => p.close);
		const last = closes[closes.length - 1]!;
		const prev = closes[closes.length - 2]!;
		return {
			last,
			change: last - prev,
			changePct: (last - prev) / prev,
			high: Math.max(...closes),
			low: Math.min(...closes),
			rv20: realizedVol(closes, 20),
			rv60: realizedVol(closes, 60),
			asOf: pts[pts.length - 1]!.date,
		};
	}, [history.data]);

	return (
		<div className="flex flex-col gap-4">
			<div className="flex flex-wrap items-center gap-3">
				<div className="w-56">
					<Combobox
						options={(tickers.data?.tickers ?? []).map((t) => ({ value: t }))}
						value={ticker || null}
						onChange={onTicker}
						placeholder="Select a ticker"
					/>
				</div>
				<div className="flex gap-1">
					{RANGES.map((r) => (
						<button
							key={r}
							type="button"
							onClick={() => setRange(r)}
							className={
								"rounded-sm border px-2 py-1 text-xs " +
								(range === r
									? "border-accent text-ink"
									: "border-hairline text-ink-secondary")
							}
						>
							{r}
						</button>
					))}
				</div>
				{stats && (
					<Freshness
						at={stats.asOf}
						label="data from"
						staleAfterSeconds={3 * 86400}
					/>
				)}
			</div>

			{stats && (
				<MetricRow>
					<Metric
						label="Last"
						value={<NumberCell value={stats.last} magnitude="price" />}
						change={<DeltaBadge value={stats.changePct} magnitude="rate" />}
					/>
					<Metric
						label="Period high"
						value={<NumberCell value={stats.high} magnitude="price" />}
					/>
					<Metric
						label="Period low"
						value={<NumberCell value={stats.low} magnitude="price" />}
					/>
					<Metric
						label="Realised vol 20d"
						value={<NumberCell value={stats.rv20} magnitude="vol" />}
					/>
					<Metric
						label="Realised vol 60d"
						value={<NumberCell value={stats.rv60} magnitude="vol" />}
						footnote="the bridge to implied vol"
					/>
				</MetricRow>
			)}

			<PriceSeriesChart
				title={ticker ? `${ticker} — ${range}` : "Price"}
				isLoading={history.isLoading}
				error={history.error}
				candles={history.data?.points.map((p) => ({
					time: p.date,
					open: p.close,
					high: p.close,
					low: p.close,
					close: p.close,
				}))}
			/>
		</div>
	);
}
