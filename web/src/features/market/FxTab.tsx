import { type FxCurrency, useFxHistory, useFxOverview } from "@/shared/api";
import { Metric, MetricRow, NumberCell } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { PriceSeriesChart } from "@/shared/viz";
import { FxCorrelationPanel } from "./FxCorrelationPanel";
import { RatesMethodology } from "./RatesMethodology";

const CCYS: FxCurrency[] = ["EUR", "USD", "GBP", "JPY", "CHF"];

function CcyToggle({
	label,
	value,
	disabled,
	onChange,
}: {
	label: string;
	value: FxCurrency;
	disabled: FxCurrency;
	onChange: (c: FxCurrency) => void;
}) {
	return (
		<div className="flex items-center gap-2 text-xs text-ink-secondary">
			<span>{label}</span>
			<div role="group" aria-label={label} className="flex gap-1">
				{CCYS.map((c) => (
					<button
						key={c}
						type="button"
						disabled={c === disabled}
						aria-pressed={c === value}
						onClick={() => onChange(c)}
						className={
							"rounded-sm border px-2 py-1 text-xs disabled:opacity-30 " +
							(c === value
								? "border-accent text-ink"
								: "border-hairline text-ink-secondary")
						}
					>
						{c}
					</button>
				))}
			</div>
		</div>
	);
}

const pct = (v: number | null | undefined) =>
	v == null ? "—" : `${(v * 100).toFixed(2)}%`;

/**
 * FX tab: spot from the ECB reference rates, forwards from covered interest
 * parity on the Rates tab's curves, historical volatility, and the
 * correlation of any stored ticker with the pair — the inputs of a quanto.
 * The methodology is served with the data (api/app/fx.py).
 */
export function FxTab({
	base,
	quote,
	onPair,
}: {
	base: FxCurrency;
	quote: FxCurrency;
	onPair: (base: FxCurrency, quote: FxCurrency) => void;
}) {
	const overview = useFxOverview(base, quote);
	const history = useFxHistory(base, quote, 5);
	const d = overview.data;
	const pair = `${base}/${quote}`;

	return (
		<div className="flex flex-col gap-6">
			<div className="flex flex-wrap items-center gap-4">
				<CcyToggle
					label="Base"
					value={base}
					disabled={quote}
					onChange={(c) => onPair(c, quote)}
				/>
				<CcyToggle
					label="Quote"
					value={quote}
					disabled={base}
					onChange={(c) => onPair(base, c)}
				/>
			</div>

			{overview.error ? (
				<ErrorState error={overview.error} onRetry={() => overview.refetch()} />
			) : !d ? (
				<ChartSkeleton />
			) : (
				<>
					<MetricRow>
						<Metric
							label={`${pair} spot`}
							value={<NumberCell value={d.spot} magnitude="price" />}
							footnote={`ECB fixing ${d.spot_date}`}
						/>
						{Object.entries(d.realised_vol).map(([w, v]) => (
							<Metric
								key={w}
								label={`Realised vol ${w}`}
								value={pct(v)}
								footnote="historical, daily"
							/>
						))}
					</MetricRow>
					{d.warnings.map((w) => (
						<p key={w} className="text-xs text-warning">
							{w}
						</p>
					))}
					<PriceSeriesChart
						title={`${pair} — 5Y (${quote} per ${base})`}
						isLoading={history.isLoading}
						error={history.error}
						height={260}
						candles={history.data?.points.map((p) => ({
							time: p.date,
							open: p.rate,
							high: p.rate,
							low: p.rate,
							close: p.rate,
						}))}
					/>
					<section className="flex flex-col gap-2">
						<h2 className="text-sm font-semibold text-ink">
							Forwards (covered interest parity)
						</h2>
						{d.forwards ? (
							<div className="overflow-x-auto rounded-sm border border-hairline">
								<table className="w-full text-sm tabular-nums">
									<thead className="bg-surface-raised text-left text-xs text-ink-secondary">
										<tr>
											<th className="px-3 py-2 font-medium">Tenor</th>
											<th className="px-3 py-2 text-right font-medium">
												Forward
											</th>
											<th className="px-3 py-2 text-right font-medium">
												Points (F − S)
											</th>
										</tr>
									</thead>
									<tbody>
										{d.forwards.map((f) => (
											<tr key={f.label} className="border-t border-hairline">
												<td className="px-3 py-1.5 text-ink">{f.label}</td>
												<td className="px-3 py-1.5 text-right text-ink">
													{f.forward.toFixed(5)}
												</td>
												<td className="px-3 py-1.5 text-right text-ink-secondary">
													{f.points >= 0 ? "+" : ""}
													{f.points.toFixed(5)}
												</td>
											</tr>
										))}
									</tbody>
								</table>
							</div>
						) : (
							<p className="rounded-sm border border-hairline bg-surface p-3 text-sm text-ink-secondary">
								{d.forwards_unavailable}
							</p>
						)}
						{Object.keys(d.curve_dates).length > 0 && (
							<p className="text-xs text-ink-muted">
								Curves as of{" "}
								{Object.entries(d.curve_dates)
									.map(([c, dt]) => `${c} ${dt}`)
									.join(", ")}
								; spot {d.spot_date}. Theoretical: no cross-currency basis.
							</p>
						)}
					</section>
					<FxCorrelationPanel base={base} quote={quote} />
					<RatesMethodology sections={d.methodology} />
				</>
			)}
		</div>
	);
}
