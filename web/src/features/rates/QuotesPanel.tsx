import type { RatesAnalysisRequest } from "@/shared/api";
import { Field } from "@/shared/ui";
import { QuoteTable } from "./QuoteTable";

type Req = RatesAnalysisRequest;

/**
 * The market inputs of the analysis, all editable: the OIS curve (deposits
 * and par OIS swaps), the floating index (FRAs and par swaps against it), and
 * the ATM swaption normal vols Hull-White is calibrated to.
 */
export function QuotesPanel({
	value,
	onChange,
}: {
	value: Req;
	onChange: (next: Req) => void;
}) {
	const set = <K extends keyof Req>(key: K, v: Req[K]) =>
		onChange({ ...value, [key]: v });
	const a = value.hull_white_mean_reversion;
	return (
		<section className="flex flex-col gap-4 rounded-sm border border-hairline bg-surface p-3">
			<h2 className="text-sm font-semibold text-ink">Quotes</h2>
			<div className="grid gap-4 sm:grid-cols-2">
				<QuoteTable
					title="OIS deposits"
					columns={[
						{ key: "tenor", label: "Tenor", unit: "years" },
						{ key: "rate", label: "Rate", unit: "pct" },
					]}
					rows={value.deposits ?? []}
					minRows={0}
					onChange={(r) => set("deposits", r)}
				/>
				<QuoteTable
					title="Par OIS swaps"
					columns={[
						{ key: "tenor", label: "Tenor", unit: "years" },
						{ key: "rate", label: "Rate", unit: "pct" },
					]}
					rows={value.ois}
					onChange={(r) => set("ois", r)}
				/>
				<QuoteTable
					title="Index FRAs"
					columns={[
						{ key: "start", label: "Start", unit: "years" },
						{ key: "end", label: "End", unit: "years" },
						{ key: "rate", label: "Rate", unit: "pct" },
					]}
					rows={value.fras ?? []}
					minRows={0}
					onChange={(r) => set("fras", r)}
				/>
				<QuoteTable
					title="Par swaps on the index"
					columns={[
						{ key: "tenor", label: "Tenor", unit: "years" },
						{ key: "rate", label: "Rate", unit: "pct" },
					]}
					rows={value.swaps}
					onChange={(r) => set("swaps", r)}
				/>
			</div>
			<QuoteTable
				title="ATM swaption normal vols"
				columns={[
					{ key: "expiry", label: "Expiry", unit: "years" },
					{ key: "tenor", label: "Tenor", unit: "years" },
					{ key: "normal_vol", label: "Vol", unit: "bp" },
				]}
				rows={value.swaption_vols}
				onChange={(r) => set("swaption_vols", r)}
			/>
			<div className="grid gap-3 sm:grid-cols-3">
				<Field
					label="Fixed payments / year"
					type="number"
					min={1}
					max={12}
					value={value.fixed_frequency}
					onChange={(e) => set("fixed_frequency", Number(e.target.value) || 1)}
				/>
				<Field
					label="Index payments / year"
					type="number"
					min={1}
					max={12}
					value={value.float_frequency}
					onChange={(e) => set("float_frequency", Number(e.target.value) || 1)}
				/>
				<Field
					label="Hull-White a (blank: fit)"
					type="number"
					step="0.005"
					min={0.0001}
					max={1}
					value={a ?? ""}
					onChange={(e) =>
						set(
							"hull_white_mean_reversion",
							e.target.value === "" ? null : Number(e.target.value),
						)
					}
				/>
			</div>
		</section>
	);
}
