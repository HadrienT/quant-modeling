import { useState } from "react";
import { AlertTriangle } from "lucide-react";
import { type RateCurrency, useRatesOverview } from "@/shared/api";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { GovernmentCurvePanel } from "./GovernmentCurvePanel";
import { RatesMethodology } from "./RatesMethodology";
import { ReferenceRatesPanel } from "./ReferenceRatesPanel";

const CURRENCIES: RateCurrency[] = ["USD", "EUR", "GBP", "CHF", "JPY"];
const FORWARD_PERIODS = [
	{ years: 0.25, label: "3M" },
	{ years: 0.5, label: "6M" },
	{ years: 1, label: "1Y" },
];

function Toggle<T extends string | number>({
	options,
	value,
	onChange,
	label,
}: {
	options: { value: T; label: string }[];
	value: T;
	onChange: (v: T) => void;
	label: string;
}) {
	return (
		<div role="group" aria-label={label} className="flex gap-1">
			{options.map((o) => (
				<button
					key={String(o.value)}
					type="button"
					aria-pressed={o.value === value}
					onClick={() => onChange(o.value)}
					className={
						"rounded-sm border px-2 py-1 text-xs " +
						(o.value === value
							? "border-accent text-ink"
							: "border-hairline text-ink-secondary")
					}
				>
					{o.label}
				</button>
			))}
		</div>
	);
}

/**
 * Rates tab: per currency, the government curve (the only free full term
 * structure), the reference rates (fixings and published averages — listed,
 * never drawn on a tenor axis), and the methodology, always visible. Every
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
					{data.warnings.length > 0 && (
						<ul className="flex flex-col gap-1 rounded-sm border border-warning/40 bg-surface p-3 text-xs text-warning">
							{data.warnings.map((w) => (
								<li key={w} className="flex gap-2">
									<AlertTriangle className="size-3.5 shrink-0" aria-hidden />
									{w}
								</li>
							))}
						</ul>
					)}
					<GovernmentCurvePanel
						curve={data.government}
						unavailable={data.government_unavailable}
					/>
					<ReferenceRatesPanel
						currency={currency}
						benchmarks={data.benchmarks}
						headline={data.headline_series}
					/>
					<RatesMethodology sections={data.methodology} />
				</>
			)}
		</div>
	);
}
