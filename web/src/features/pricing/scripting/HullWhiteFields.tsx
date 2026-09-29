import type { Schemas } from "@/shared/api";
import { Field } from "@/shared/ui";

export type HullWhiteInputs = Schemas["HullWhiteInputs"];

export const DEFAULT_HULL_WHITE: HullWhiteInputs = {
	mean_reversion: 0.03,
	sigma: 0.01,
	rho: 0,
	currency: "USD",
};

const CURRENCIES: HullWhiteInputs["currency"][] = ["USD", "EUR", "GBP", "JPY"];

/** The short rate of the Hull-White model: fitted to the currency's
 * government discount curve from the database, its mean reversion, its vol
 * and its correlation with the equity. df() in the script reads it. */
export function HullWhiteFields({
	value,
	onChange,
}: {
	value: HullWhiteInputs;
	onChange: (v: HullWhiteInputs) => void;
}) {
	const set = (patch: Partial<HullWhiteInputs>) =>
		onChange({ ...value, ...patch });
	return (
		<fieldset className="grid grid-cols-2 gap-3 sm:grid-cols-4">
			<legend className="col-span-full mb-1 text-2xs text-ink-muted uppercase">
				Hull-White short rate
			</legend>
			<label className="flex flex-col gap-1 text-sm">
				<span className="text-2xs text-ink-muted uppercase">Curve</span>
				<select
					className="rounded border border-hairline bg-surface px-2 py-1"
					value={value.currency}
					onChange={(e) =>
						set({ currency: e.target.value as HullWhiteInputs["currency"] })
					}
				>
					{CURRENCIES.map((c) => (
						<option key={c} value={c}>
							{c} government
						</option>
					))}
				</select>
			</label>
			<Field
				label="Mean reversion a"
				type="number"
				step="0.01"
				min={0.0001}
				max={1}
				value={value.mean_reversion}
				onChange={(e) => set({ mean_reversion: Number(e.target.value) })}
			/>
			<Field
				label="Rate vol σ (bp)"
				type="number"
				step="5"
				min={1}
				value={Math.round((value.sigma ?? 0.01) * 1e4 * 100) / 100}
				onChange={(e) => set({ sigma: Number(e.target.value) / 1e4 })}
			/>
			<Field
				label="Equity-rate correlation"
				type="number"
				step="0.1"
				min={-1}
				max={1}
				value={value.rho}
				onChange={(e) => set({ rho: Number(e.target.value) })}
			/>
		</fieldset>
	);
}
