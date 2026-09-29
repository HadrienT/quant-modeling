import type { RatesAnalysisRequest, RatesAnalysisResponse } from "@/shared/api";
import { Field, Metric, MetricRow, Segmented } from "@/shared/ui";
import { SwaptionModelsTable } from "./SwaptionModelsTable";
import { money, pct } from "./format";

type SwaptionInput = RatesAnalysisRequest["swaption"];

const num = (s: string) => (s === "" ? null : Number(s));

/**
 * One swaption, under every model on the same multi-curve forward and
 * annuity, and its Bermudan twin under the calibrated Hull-White.
 */
export function SwaptionPanel({
	value,
	onChange,
	result,
}: {
	value: SwaptionInput;
	onChange: (next: SwaptionInput) => void;
	result: RatesAnalysisResponse["swaption"] | undefined;
}) {
	const set = (patch: Partial<SwaptionInput>) =>
		onChange({ ...value, ...patch });
	const sabr = value.sabr;
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">Swaption</h2>
			<div className="grid items-end gap-3 sm:grid-cols-5">
				<Segmented
					label="Swaption type"
					options={[
						{ value: "payer", label: "Payer" },
						{ value: "receiver", label: "Receiver" },
					]}
					value={value.payer ? "payer" : "receiver"}
					onChange={(v) => set({ payer: v === "payer" })}
				/>
				<Field
					label="Expiry (years)"
					type="number"
					step="1"
					min={1}
					value={value.expiry}
					onChange={(e) => set({ expiry: Number(e.target.value) })}
				/>
				<Field
					label="Swap tenor (years)"
					type="number"
					step="1"
					min={1}
					value={value.tenor}
					onChange={(e) => set({ tenor: Number(e.target.value) })}
				/>
				<Field
					label="Strike (%, blank: ATM)"
					type="number"
					step="0.01"
					value={
						value.strike == null ? "" : +(value.strike * 100).toPrecision(10)
					}
					onChange={(e) => {
						const v = num(e.target.value);
						set({ strike: v == null ? null : v / 100 });
					}}
				/>
				<Field
					label="Notional"
					type="number"
					step="1000000"
					min={1}
					value={value.notional}
					onChange={(e) => set({ notional: Number(e.target.value) })}
				/>
				<Field
					label="Normal vol (bp, blank: grid)"
					type="number"
					step="0.5"
					value={
						value.normal_vol == null
							? ""
							: +(value.normal_vol * 1e4).toPrecision(10)
					}
					onChange={(e) => {
						const v = num(e.target.value);
						set({ normal_vol: v == null ? null : v / 1e4 });
					}}
				/>
				<Field
					label="Black vol (%)"
					type="number"
					step="0.5"
					value={
						value.lognormal_vol == null
							? ""
							: +(value.lognormal_vol * 100).toPrecision(10)
					}
					onChange={(e) => {
						const v = num(e.target.value);
						set({ lognormal_vol: v == null ? null : v / 100 });
					}}
				/>
				<Field
					label="Shift (%)"
					type="number"
					step="0.25"
					min={0}
					value={+((value.shift ?? 0) * 100).toPrecision(10)}
					onChange={(e) => set({ shift: Number(e.target.value) / 100 })}
				/>
				{sabr && (
					<>
						<Field
							label="SABR α"
							type="number"
							step="0.001"
							value={sabr.alpha}
							onChange={(e) =>
								set({ sabr: { ...sabr, alpha: Number(e.target.value) } })
							}
						/>
						<Field
							label="SABR ρ"
							type="number"
							step="0.05"
							min={-0.99}
							max={0.99}
							value={sabr.rho}
							onChange={(e) =>
								set({ sabr: { ...sabr, rho: Number(e.target.value) } })
							}
						/>
						<Field
							label="SABR ν"
							type="number"
							step="0.05"
							min={0}
							value={sabr.nu}
							onChange={(e) =>
								set({ sabr: { ...sabr, nu: Number(e.target.value) } })
							}
						/>
						<Field
							label="SABR β"
							type="number"
							step="0.1"
							min={0}
							max={1}
							value={sabr.beta}
							onChange={(e) =>
								set({ sabr: { ...sabr, beta: Number(e.target.value) } })
							}
						/>
					</>
				)}
			</div>
			{result && (
				<>
					<MetricRow>
						<Metric label="Forward swap rate" value={pct(result.forward)} />
						<Metric label="Strike" value={pct(result.strike)} />
						<Metric
							label="Annuity"
							value={result.annuity.toFixed(4)}
							footnote="Per unit notional, OIS-discounted"
						/>
						<Metric
							label="Bermudan (Hull-White)"
							value={money(result.bermudan_price)}
							footnote={`Annual exercise, ${result.bermudan_exercises.length} dates`}
						/>
						<Metric
							label="Switch premium"
							value={money(result.switch_premium)}
							footnote="Bermudan − European, Hull-White"
						/>
					</MetricRow>
					<SwaptionModelsTable prices={result.prices} />
				</>
			)}
		</section>
	);
}
