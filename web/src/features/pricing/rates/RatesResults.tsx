import type { SwapInput, SwaptionInput } from "@/shared/api";
import { useSwapPricing, useSwaptionPricing } from "@/shared/api";
import { ratePct, signedMoney } from "@/shared/format";
import { Metric, MetricRow } from "@/shared/ui";
import { ErrorState, MetricRowSkeleton } from "@/shared/ui/states";
import { ModelWarnings } from "../scripting/ModelWarnings";
import { CalibrationPanel } from "./CalibrationPanel";
import { CurvesPanel } from "./CurvesPanel";
import { RatesModelDecision } from "./RatesModelDecision";
import { RatesValueCard } from "./RatesValueCard";
import { SwapSchedule } from "./SwapSchedule";
import { SwaptionModelsTable } from "./SwaptionModelsTable";
import {
	type RatesQuotes,
	type RatesValues,
	swapBody,
	swaptionBody,
	useRatesInputs,
} from "./useRatesInputs";

/** Where the quotes of a price come from, for its value card. */
type Origin = {
	currency: string;
	source: "market" | "manual";
	asOf?: string | null;
};

/**
 * The results of a rates product: its value on the chosen quotes, what the
 * value decomposes into, and the curves it rests on. A quote set the store
 * cannot back (stale or missing trades) is an error that says so — never a
 * silent switch to the other set.
 */
export function RatesResults({
	kind,
	values,
}: {
	kind: "swap" | "swaption";
	values: RatesValues;
}) {
	const { set, quotes, contract, edited } = useRatesInputs(kind, values);
	if (set.error)
		return <ErrorState error={set.error} onRetry={() => set.refetch()} />;
	if (!set.data || !quotes || !contract) return <MetricRowSkeleton />;
	const origin: Origin = {
		currency: set.data.currency,
		// Edited quotes are the user's, whatever they started from.
		source: edited ? "manual" : set.data.source,
		asOf: set.data.as_of,
	};
	return (
		<div className="flex flex-col gap-4">
			{kind === "swap" ? (
				<SwapResults
					quotes={quotes}
					swap={contract as SwapInput}
					origin={origin}
				/>
			) : (
				<SwaptionResults
					quotes={quotes}
					swaption={contract as SwaptionInput}
					origin={origin}
				/>
			)}
			<CurvesPanel quotes={quotes.curves} />
		</div>
	);
}

function SwapResults({
	quotes,
	swap,
	origin,
}: {
	quotes: RatesQuotes;
	swap: SwapInput;
	origin: Origin;
}) {
	const q = useSwapPricing(swapBody(quotes, swap));
	if (q.error)
		return <ErrorState error={q.error} onRetry={() => q.refetch()} />;
	if (!q.data) return <MetricRowSkeleton />;
	const r = q.data;
	return (
		<>
			<RatesValueCard
				npv={r.npv}
				engine="analytic"
				computeMs={r.compute_ms}
				roundTripMs={r.round_trip_ms}
				diagnostics={r.diagnostics}
				{...origin}
			/>
			<MetricRow className="xl:grid-cols-3">
				<Metric
					label="Par rate"
					value={ratePct(r.swap.par_rate)}
					footnote="The fixed rate worth zero today"
				/>
				<Metric
					label="PV01"
					value={signedMoney(r.swap.pv01)}
					footnote="Annuity × notional × 1bp"
				/>
				<Metric label="Annuity" value={r.swap.annuity.toFixed(4)} />
				<Metric label="Fixed leg" value={signedMoney(r.swap.fixed_leg)} />
				<Metric label="Floating leg" value={signedMoney(r.swap.floating_leg)} />
			</MetricRow>
			<SwapSchedule swap={r.swap} />
		</>
	);
}

function SwaptionResults({
	quotes,
	swaption,
	origin,
}: {
	quotes: RatesQuotes;
	swaption: SwaptionInput;
	origin: Origin;
}) {
	const q = useSwaptionPricing(swaptionBody(quotes, swaption));
	if (q.error)
		return <ErrorState error={q.error} onRetry={() => q.refetch()} />;
	if (!q.data) return <MetricRowSkeleton />;
	const r = q.data;
	const s = r.swaption;
	return (
		<>
			<RatesValueCard
				npv={r.npv}
				engine={s.exercise === "bermudan" ? "lattice" : "analytic"}
				computeMs={r.compute_ms}
				roundTripMs={r.round_trip_ms}
				diagnostics={r.diagnostics}
				{...origin}
			/>
			<RatesModelDecision swaption={s} />
			<ModelWarnings warnings={r.warnings ?? []} />
			<MetricRow className="xl:grid-cols-3">
				<Metric label="Forward swap rate" value={ratePct(s.forward)} />
				<Metric label="Strike" value={ratePct(s.strike)} />
				<Metric
					label="Annuity"
					value={s.annuity.toFixed(4)}
					footnote="Per unit notional, OIS-discounted"
				/>
				<Metric
					label="Bermudan (Hull-White)"
					value={signedMoney(s.bermudan_price)}
					footnote={`Annual exercise, ${s.bermudan_exercises.length} dates`}
				/>
				<Metric
					label="Switch premium"
					value={signedMoney(s.switch_premium)}
					footnote="Bermudan − European, Hull-White"
				/>
			</MetricRow>
			<SwaptionModelsTable swaption={s} />
			<CalibrationPanel hw={s.hull_white} />
		</>
	);
}
