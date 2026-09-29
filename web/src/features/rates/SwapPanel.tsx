import type { RatesAnalysisRequest, RatesAnalysisResponse } from "@/shared/api";
import { Field, Metric, MetricRow, Segmented } from "@/shared/ui";
import { money, pct } from "./format";

type SwapInput = RatesAnalysisRequest["swap"];

/**
 * A vanilla swap on the two curves: the trade's terms, then its value, par
 * rate and PV01. Legs follow the cash-flow sign convention (paid = negative).
 */
export function SwapPanel({
	value,
	onChange,
	result,
}: {
	value: SwapInput;
	onChange: (next: SwapInput) => void;
	result: RatesAnalysisResponse["swap"] | undefined;
}) {
	const set = (patch: Partial<SwapInput>) => onChange({ ...value, ...patch });
	const payerSign = value.payer ? 1 : -1;
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">Swap</h2>
			<div className="grid items-end gap-3 sm:grid-cols-5">
				<Segmented
					label="Side"
					options={[
						{ value: "payer", label: "Pay fixed" },
						{ value: "receiver", label: "Receive fixed" },
					]}
					value={value.payer ? "payer" : "receiver"}
					onChange={(v) => set({ payer: v === "payer" })}
				/>
				<Field
					label="Start (years)"
					type="number"
					step="0.25"
					min={0}
					value={value.start}
					onChange={(e) => set({ start: Number(e.target.value) })}
				/>
				<Field
					label="Tenor (years)"
					type="number"
					step="1"
					min={1}
					value={value.tenor}
					onChange={(e) => set({ tenor: Number(e.target.value) })}
				/>
				<Field
					label="Fixed rate (%)"
					type="number"
					step="0.01"
					value={+(value.fixed_rate * 100).toPrecision(10)}
					onChange={(e) => set({ fixed_rate: Number(e.target.value) / 100 })}
				/>
				<Field
					label="Notional"
					type="number"
					step="1000000"
					min={1}
					value={value.notional}
					onChange={(e) => set({ notional: Number(e.target.value) })}
				/>
			</div>
			{result && (
				<MetricRow>
					<Metric label="Value" value={money(result.npv)} />
					<Metric label="Par rate" value={pct(result.par_rate)} />
					<Metric
						label="PV01"
						value={money(result.pv01)}
						footnote="Annuity × notional × 1bp"
					/>
					<Metric
						label="Fixed leg"
						value={money(-payerSign * result.fixed_leg)}
					/>
					<Metric
						label="Floating leg"
						value={money(payerSign * result.floating_leg)}
					/>
				</MetricRow>
			)}
		</section>
	);
}
