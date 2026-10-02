import { useState } from "react";
import { Button, Field } from "@/shared/ui";
import { QuoteTable } from "./QuoteTable";
import type { RatesQuotes } from "./useRatesInputs";

const TENOR_RATE = [
	{ key: "tenor", label: "Tenor", unit: "years" },
	{ key: "rate", label: "Rate", unit: "pct" },
] as const;

/**
 * The quotes behind the price, all editable: the OIS curve (deposits and par
 * OIS swaps), the floating index (FRAs and par swaps against it) and, for a
 * swaption, the ATM normal vols Hull-White is calibrated to. Edits are a
 * draft until applied: a calibration is about a second, not a keystroke.
 */
export function QuotesEditor({
	value,
	withVols,
	edited,
	onApply,
	onReset,
}: {
	value: RatesQuotes;
	/** A swaption also depends on the vols and the Hull-White mean reversion. */
	withVols: boolean;
	/** The applied quotes are no longer the set's own. */
	edited: boolean;
	onApply: (next: RatesQuotes) => void;
	onReset: () => void;
}) {
	// The applied quotes come back through the URL as a new object: compare
	// by content, and restart the draft when they change under it (a reset).
	const applied = JSON.stringify(value);
	const [draft, setDraft] = useState(value);
	const [synced, setSynced] = useState(applied);
	if (synced !== applied) {
		setSynced(applied);
		setDraft(value);
	}
	const dirty = JSON.stringify(draft) !== applied;
	const curves = draft.curves;
	const setCurves = (patch: Partial<RatesQuotes["curves"]>) =>
		setDraft({ ...draft, curves: { ...curves, ...patch } });
	const a = draft.hull_white_mean_reversion;
	return (
		<details className="rounded-sm border border-hairline bg-surface p-3">
			<summary className="cursor-pointer text-sm text-ink">
				Edit the quotes
				{edited && <span className="text-2xs text-warning"> · edited</span>}
			</summary>
			<div className="mt-3 flex flex-col gap-4">
				<QuoteTable
					title="Deposits (OIS curve)"
					hint="Cash deposit rates: the short end of the discount curve."
					columns={[...TENOR_RATE]}
					rows={curves.deposits ?? []}
					minRows={0}
					onChange={(r) => setCurves({ deposits: r })}
				/>
				<QuoteTable
					title="Par OIS swaps"
					hint="Fixed rate against the compounded overnight rate: the discount curve."
					columns={[...TENOR_RATE]}
					rows={curves.ois}
					onChange={(r) => setCurves({ ois: r })}
				/>
				<QuoteTable
					title="FRAs on the floating index"
					hint="The index fixing locked today for a future period: the short end of the projection curve."
					columns={[
						{ key: "start", label: "Start", unit: "years" },
						{ key: "end", label: "End", unit: "years" },
						{ key: "rate", label: "Rate", unit: "pct" },
					]}
					rows={curves.fras ?? []}
					minRows={0}
					onChange={(r) => setCurves({ fras: r })}
				/>
				<QuoteTable
					title="Par swaps on the floating index"
					hint="Fixed rate against the index (a rate such as EURIBOR 6M, not an equity index): the projection curve."
					columns={[...TENOR_RATE]}
					rows={curves.swaps}
					onChange={(r) => setCurves({ swaps: r })}
				/>
				{withVols && (
					<QuoteTable
						title="ATM swaption normal vols"
						hint="Hull-White is calibrated to these; Bachelier takes the nearest one."
						columns={[
							{ key: "expiry", label: "Expiry", unit: "years" },
							{ key: "tenor", label: "Tenor", unit: "years" },
							{ key: "normal_vol", label: "Vol", unit: "bp" },
						]}
						rows={draft.swaption_vols}
						onChange={(r) => setDraft({ ...draft, swaption_vols: r })}
					/>
				)}
				<div className="grid grid-cols-2 gap-2">
					<Field
						label="Fixed payments / year"
						type="number"
						min={1}
						max={12}
						value={curves.fixed_frequency}
						onChange={(e) =>
							setCurves({ fixed_frequency: Number(e.target.value) || 1 })
						}
					/>
					<Field
						label="Index payments / year"
						type="number"
						min={1}
						max={12}
						value={curves.float_frequency}
						onChange={(e) =>
							setCurves({ float_frequency: Number(e.target.value) || 1 })
						}
					/>
					{withVols && (
						<Field
							label="Hull-White a (blank: fit)"
							type="number"
							step="0.005"
							min={0.0001}
							max={1}
							value={a ?? ""}
							onChange={(e) =>
								setDraft({
									...draft,
									hull_white_mean_reversion:
										e.target.value === "" ? null : Number(e.target.value),
								})
							}
						/>
					)}
				</div>
				<div className="flex flex-wrap items-center gap-2">
					<Button size="sm" disabled={!dirty} onClick={() => onApply(draft)}>
						Reprice on these quotes
					</Button>
					<Button
						size="sm"
						variant="secondary"
						disabled={!edited && !dirty}
						onClick={onReset}
					>
						Reset the quotes
					</Button>
					{dirty && (
						<span className="text-2xs text-ink-muted">
							Edited: reprice to update the results.
						</span>
					)}
				</div>
			</div>
		</details>
	);
}
