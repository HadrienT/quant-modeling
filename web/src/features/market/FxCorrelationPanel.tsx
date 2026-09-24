import { useState } from "react";
import { Link } from "@tanstack/react-router";
import {
	type FxCurrency,
	type MarketId,
	useFxCorrelation,
	useMarkets,
	useTickers,
} from "@/shared/api";
import { encodeParams } from "@/shared/products";
import { Metric, MetricRow } from "@/shared/ui";
import { MarketTickerPicker } from "./MarketTickerPicker";

const WINDOWS = ["1Y", "3Y", "5Y"] as const;
const FREQS = ["weekly", "daily"] as const;

function Seg<T extends string>({
	options,
	value,
	onChange,
	label,
}: {
	options: readonly T[];
	value: T;
	onChange: (v: T) => void;
	label: string;
}) {
	return (
		<div role="group" aria-label={label} className="flex gap-1">
			{options.map((o) => (
				<button
					key={o}
					type="button"
					aria-pressed={o === value}
					onClick={() => onChange(o)}
					className={
						"rounded-sm border px-2 py-0.5 text-xs " +
						(o === value
							? "border-accent text-ink"
							: "border-hairline text-ink-secondary")
					}
				>
					{o}
				</button>
			))}
		</div>
	);
}

/**
 * Historical correlation of any stored ticker with the FX pair — the ρ of a
 * quanto. Weekly by default: the series are fixed at different hours, and
 * daily returns of asynchronous series understate correlation (Epps).
 */
export function FxCorrelationPanel({
	base,
	quote,
}: {
	base: FxCurrency;
	quote: FxCurrency;
}) {
	const [market, setMarket] = useState<MarketId>("CAC40");
	const [ticker, setTicker] = useState<string | null>(null);
	const [window, setWindow] = useState<(typeof WINDOWS)[number]>("3Y");
	const [frequency, setFrequency] = useState<(typeof FREQS)[number]>("weekly");
	const markets = useMarkets();
	const members = useTickers(market).data?.members ?? [];
	const chosen = ticker ?? members[0]?.ticker ?? "";
	const corr = useFxCorrelation(
		chosen ? { ticker: chosen, base, quote, window, frequency } : null,
	);
	const c = corr.data;
	const f2 = (v: number) => v.toFixed(2);
	const assetCcy = members.find((m) => m.ticker === chosen)?.currency;
	// A quanto on this asset paid in QUOTE needs the pair ASSET-CCY/QUOTE.
	const quantoReady = !!c && assetCcy === base;
	const quantoParams = c
		? encodeParams({
				spot: +c.asset_last.toFixed(4),
				strike: +c.asset_last.toFixed(4),
				vol: +(c.asset_vol * 100).toFixed(2),
				fx_vol: +(c.fx_vol * 100).toFixed(2),
				correlation: +c.correlation.toFixed(3),
				fx_rate: +c.fx_last.toFixed(6),
			})
		: "";

	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">
				Correlation with {base}/{quote}
			</h2>
			<div className="flex flex-wrap items-center gap-3">
				<MarketTickerPicker
					markets={markets.data?.markets ?? []}
					market={market}
					members={members}
					ticker={chosen}
					onMarket={(m) => {
						setMarket(m);
						setTicker(null);
					}}
					onTicker={setTicker}
				/>
				<Seg
					label="Window"
					options={WINDOWS}
					value={window}
					onChange={setWindow}
				/>
				<Seg
					label="Returns"
					options={FREQS}
					value={frequency}
					onChange={setFrequency}
				/>
			</div>
			{corr.error ? (
				<p className="text-sm text-ink-secondary">
					{(corr.error as Error).message}
				</p>
			) : c ? (
				<>
					<MetricRow>
						<Metric
							label="Correlation"
							value={f2(c.correlation)}
							footnote={`95% CI [${f2(c.ci_low)}, ${f2(c.ci_high)}]`}
						/>
						<Metric
							label="Returns used"
							value={String(c.n)}
							footnote={`${c.frequency}, ${c.start} → ${c.end}`}
						/>
						<Metric
							label={`${c.ticker} vol`}
							value={`${(c.asset_vol * 100).toFixed(1)}%`}
							footnote="historical, annualised"
						/>
						<Metric
							label={`${base}/${quote} vol`}
							value={`${(c.fx_vol * 100).toFixed(1)}%`}
							footnote="historical, annualised"
						/>
					</MetricRow>
					{quantoReady ? (
						<p className="text-xs text-ink-muted">
							<Link
								to="/price"
								search={{ product: "quanto", p: quantoParams }}
								className="text-accent underline"
							>
								Price a quanto on {c.ticker} paid in {quote}
							</Link>{" "}
							with these historical estimates (at-the-money, fixed rate = last
							fixing; set the two rates and the maturity there).
						</p>
					) : (
						<p className="text-xs text-ink-muted">
							For a quanto, use the pair quoted as payment currency per unit of
							the asset&apos;s currency
							{assetCcy
								? ` (${c.ticker} is in ${assetCcy}: ${assetCcy}/…)`
								: ""}
							.
						</p>
					)}
				</>
			) : null}
		</section>
	);
}
