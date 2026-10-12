import { useState } from "react";
import { Plus, Trash2 } from "lucide-react";
import type { Counterparty, CsaTerms, NettingSet } from "@/shared/api";
import {
	DEFAULT_CSA,
	RATINGS,
	SECTORS,
	newNettingSet,
} from "@/shared/portfolio";
import { Button, Input, Label } from "@/shared/ui";

const box =
	"h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink";

/** The amounts and the period of a collateral agreement. */
const CSA_FIELDS: [keyof CsaTerms & string, string][] = [
	["threshold_counterparty", "Threshold, counterparty"],
	["threshold_bank", "Threshold, bank"],
	["minimum_transfer_amount", "Minimum transfer amount"],
	["margin_period_of_risk_days", "Margin period of risk (business days)"],
];

/**
 * Create or edit a counterparty: its name, the rating its credit curve is
 * read from, its sector, the recovery assumed, and its netting sets with
 * their collateral terms. A netting set still traded under cannot be removed.
 */
export function CounterpartyForm({
	initial,
	inUse,
	onSave,
	onCancel,
}: {
	initial: Counterparty;
	/** ids of the netting sets a trade is under */
	inUse: Set<string>;
	onSave: (c: Counterparty) => void;
	onCancel: () => void;
}) {
	const [c, setC] = useState(initial);
	const sets = c.netting_sets ?? [];
	const setNettingSet = (id: string, change: Partial<NettingSet>) =>
		setC({
			...c,
			netting_sets: sets.map((n) => (n.id === id ? { ...n, ...change } : n)),
		});
	const recovery = c.recovery ?? 0.4;
	const valid =
		c.name.trim() !== "" &&
		recovery >= 0 &&
		recovery <= 0.9 &&
		sets.length > 0 &&
		sets.every(
			(n) =>
				n.name.trim() !== "" &&
				(!n.csa ||
					(CSA_FIELDS.every(([k]) => Number(n.csa![k]) >= 0) &&
						n.csa.margin_period_of_risk_days >= 1 &&
						n.csa.margin_period_of_risk_days <= 60)),
		);

	return (
		<div className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4">
			<div className="grid grid-cols-1 gap-3 sm:grid-cols-4">
				<label className="flex flex-col gap-1">
					<Label>Name</Label>
					<Input
						value={c.name}
						maxLength={80}
						onChange={(e) => setC({ ...c, name: e.target.value })}
					/>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Rating</Label>
					<select
						className={box}
						value={c.rating}
						onChange={(e) =>
							setC({ ...c, rating: e.target.value as Counterparty["rating"] })
						}
					>
						{RATINGS.map((r) => (
							<option key={r}>{r}</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Sector</Label>
					<select
						className={box}
						value={c.sector}
						onChange={(e) =>
							setC({ ...c, sector: e.target.value as Counterparty["sector"] })
						}
					>
						{SECTORS.map((s) => (
							<option key={s.value} value={s.value}>
								{s.label}
							</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Recovery (%)</Label>
					<Input
						type="number"
						min={0}
						max={90}
						step="any"
						value={Math.round(recovery * 1e4) / 100}
						onChange={(e) =>
							setC({ ...c, recovery: Number(e.target.value) / 100 })
						}
					/>
				</label>
			</div>

			{sets.map((n, i) => (
				<fieldset
					key={n.id}
					className="flex flex-col gap-3 rounded-sm border border-hairline p-3"
				>
					<legend className="px-1 text-xs text-ink-secondary">
						Netting set {i + 1}
					</legend>
					<div className="flex flex-wrap items-end gap-3">
						<label className="flex min-w-60 flex-1 flex-col gap-1">
							<Label>Agreement</Label>
							<Input
								value={n.name}
								maxLength={80}
								onChange={(e) => setNettingSet(n.id, { name: e.target.value })}
							/>
						</label>
						<label className="flex h-9 items-center gap-2 text-sm text-ink">
							<input
								type="checkbox"
								checked={!!n.csa}
								onChange={(e) =>
									setNettingSet(n.id, {
										csa: e.target.checked ? DEFAULT_CSA : null,
									})
								}
							/>
							Collateralised (CSA)
						</label>
						<Button
							type="button"
							size="sm"
							variant="ghost"
							disabled={inUse.has(n.id) || sets.length === 1}
							title={
								inUse.has(n.id)
									? "A trade is under this netting set: move it first"
									: "Remove this netting set"
							}
							onClick={() =>
								setC({ ...c, netting_sets: sets.filter((x) => x.id !== n.id) })
							}
						>
							<Trash2 className="size-3.5" /> Remove
						</Button>
					</div>
					{n.csa && (
						<div className="grid grid-cols-2 gap-3 sm:grid-cols-5">
							{CSA_FIELDS.map(([k, label]) => (
								<label key={k} className="flex flex-col gap-1">
									<Label>{label}</Label>
									<Input
										type="number"
										min={0}
										step="any"
										value={Number(n.csa![k])}
										onChange={(e) =>
											setNettingSet(n.id, {
												csa: { ...n.csa!, [k]: Number(e.target.value) },
											})
										}
									/>
								</label>
							))}
							<label className="flex items-center gap-2 text-sm text-ink">
								<input
									type="checkbox"
									checked={n.csa.initial_margin}
									onChange={(e) =>
										setNettingSet(n.id, {
											csa: { ...n.csa!, initial_margin: e.target.checked },
										})
									}
								/>
								Initial margin, both ways
							</label>
						</div>
					)}
				</fieldset>
			))}

			<div className="flex flex-wrap gap-2">
				<Button
					type="button"
					size="sm"
					variant="secondary"
					disabled={sets.length >= 10}
					onClick={() =>
						setC({ ...c, netting_sets: [...sets, newNettingSet()] })
					}
				>
					<Plus className="size-3.5" /> Add netting set
				</Button>
				<Button
					type="button"
					size="sm"
					disabled={!valid}
					onClick={() => onSave({ ...c, name: c.name.trim() })}
				>
					Save counterparty
				</Button>
				<Button type="button" size="sm" variant="ghost" onClick={onCancel}>
					Cancel
				</Button>
			</div>
		</div>
	);
}
