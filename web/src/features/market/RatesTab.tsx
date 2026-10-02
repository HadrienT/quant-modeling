import { useState } from "react";
import { type RateCurrency, useRatesOverview } from "@/shared/api";
import { Segmented as Toggle, WarningList } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { GovernmentCurvePanel } from "./GovernmentCurvePanel";
import { RatesMethodology } from "./RatesMethodology";
import { ReferenceRatesPanel } from "./ReferenceRatesPanel";
import { SwapMarketPanel } from "./SwapMarketPanel";

const CURRENCIES: RateCurrency[] = ["USD", "EUR", "GBP", "CHF", "JPY"];
const FORWARD_PERIODS = [
	{ years: 0.25, label: "3M" },
	{ years: 0.5, label: "6M" },
	{ years: 1, label: "1Y" },
];

/**
 * Rates tab: per currency, the government curve, the reference rates
 * (fixings and published averages — listed, never drawn on a tenor axis),
 * for USD the SOFR swap curve and swaption vols built from traded prices,
 * and the methodology, always visible. Every
 * number and every sentence of the methodology comes from the API
 * (api/app/rates.py), so the page cannot describe a computation it does not do.
 */
export function RatesTab({
	currency,
	onCurrency,
}: {
	currency: RateCurrency;
	onCurrency: (c: RateCurrency) => void;
}) {
	const [forwardPeriod, setForwardPeriod] = useState(0.5);
	const overview = useRatesOverview(currency, forwardPeriod);
	const data = overview.data;

	return (
		<div className="flex flex-col gap-6">
			<div className="flex flex-wrap items-center gap-4">
				<Toggle
					label="Currency"
					options={CURRENCIES.map((c) => ({ value: c, label: c }))}
					value={currency}
					onChange={onCurrency}
				/>
				{data?.government?.forward && (
					<div className="flex items-center gap-2 text-xs text-ink-secondary">
						<span>Forward period</span>
						<Toggle
							label="Forward period"
							options={FORWARD_PERIODS.map((f) => ({
								value: f.years,
								label: f.label,
							}))}
							value={forwardPeriod}
							onChange={setForwardPeriod}
						/>
					</div>
				)}
			</div>

			{overview.error ? (
				<ErrorState error={overview.error} onRetry={() => overview.refetch()} />
			) : !data ? (
				<ChartSkeleton />
			) : (
				<>
					<WarningList warnings={data.warnings} />
					<GovernmentCurvePanel
						curve={data.government}
						unavailable={data.government_unavailable}
					/>
					<ReferenceRatesPanel
						currency={currency}
						benchmarks={data.benchmarks}
						headline={data.headline_series}
					/>
					{/* The only currency whose swaps are published trade by trade. */}
					{currency === "USD" && <SwapMarketPanel />}
					<RatesMethodology sections={data.methodology} />
				</>
			)}
		</div>
	);
}
