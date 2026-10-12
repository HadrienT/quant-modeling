import { useState } from "react";
import { Plus } from "lucide-react";
import type { Counterparty, Portfolio } from "@/shared/api";
import { useCounterpartyExamples } from "@/shared/api";
import {
	type Ledger,
	addExamples,
	assignNettingSet,
	newCounterparty,
	nettingSetOf,
	removeCounterparty,
	saveCounterparty,
	tradedUnder,
} from "@/shared/portfolio";
import { Button } from "@/shared/ui";
import { CounterpartyCard } from "./CounterpartyCard";
import { CounterpartyForm } from "./CounterpartyForm";
import { NettingSetSelect } from "./NettingSetSelect";

/**
 * Who the over-the-counter derivatives of the portfolio face: its
 * counterparties, their netting sets, and which netting set each derivative
 * is traded under. Read-only for a demo.
 */
export function CounterpartiesTab({
	pf,
	onSave,
}: {
	pf: Portfolio;
	/** absent for a read-only portfolio */
	onSave?: (ledger: Ledger) => void;
}) {
	const [editing, setEditing] = useState<Counterparty | null>(null);
	const examples = useCounterpartyExamples();
	const counterparties = pf.counterparties ?? [];
	const derivatives = (pf.instruments ?? []).filter(
		(i) => i.spec.kind !== "equity",
	);
	const missing = (examples.data ?? []).filter(
		(e) => !counterparties.some((c) => c.id === e.id),
	);
	const inUse = new Set(
		counterparties
			.flatMap((c) => c.netting_sets ?? [])
			.filter((n) => tradedUnder(pf, n.id).length > 0)
			.map((n) => n.id),
	);

	return (
		<div className="flex flex-col gap-4">
			<p className="max-w-3xl text-sm leading-relaxed text-ink-secondary">
				An over-the-counter derivative is a contract with a counterparty, which
				may default while it owes money. Trades under the same netting set close
				out as one net amount, less the collateral its agreement provides. A
				listed share is cleared and faces no one. A counterparty&apos;s credit
				curve is the one of its rating. The exposure and the xVA of these
				netting sets are not computed on this page yet: the xVA page shows them
				on sample netting sets.
			</p>

			{onSave && !editing && (
				<div className="flex flex-wrap gap-2">
					<Button size="sm" onClick={() => setEditing(newCounterparty())}>
						<Plus className="size-3.5" /> New counterparty
					</Button>
					<Button
						size="sm"
						variant="secondary"
						disabled={missing.length === 0}
						title="Fictional counterparties, one per kind a derivatives desk faces, with the collateral terms that kind usually has"
						onClick={() => onSave(addExamples(pf, missing))}
					>
						Add example counterparties
					</Button>
				</div>
			)}
			{onSave && editing && (
				<CounterpartyForm
					key={editing.id}
					initial={editing}
					inUse={inUse}
					onCancel={() => setEditing(null)}
					onSave={(c) => {
						onSave(saveCounterparty(pf, c));
						setEditing(null);
					}}
				/>
			)}

			{counterparties.length === 0 ? (
				<p className="text-sm text-ink-muted">No counterparty yet.</p>
			) : (
				counterparties.map((c) => (
					<CounterpartyCard
						key={c.id}
						pf={pf}
						counterparty={c}
						onEdit={onSave && (() => setEditing(c))}
						onDelete={
							onSave &&
							(() => {
								const next = removeCounterparty(pf, c.id);
								if (next) onSave(next);
							})
						}
					/>
				))
			)}

			{derivatives.length > 0 && (
				<section
					aria-label="Derivatives and their netting set"
					className="overflow-x-auto rounded-md border border-hairline bg-surface"
				>
					<table className="w-full text-xs">
						<thead>
							<tr className="text-2xs text-ink-muted uppercase">
								<th className="p-2 text-left">Derivative</th>
								<th className="p-2 text-left">Traded under</th>
							</tr>
						</thead>
						<tbody>
							{derivatives.map((i) => {
								const under = nettingSetOf(pf, i.netting_set_id);
								return (
									<tr key={i.id} className="border-t border-hairline">
										<td className="p-2 text-left text-ink">
											{i.label || i.id}
										</td>
										<td className="p-2 text-left text-ink-secondary">
											{onSave ? (
												<NettingSetSelect
													label={`Netting set of ${i.label || i.id}`}
													counterparties={counterparties}
													value={i.netting_set_id ?? null}
													onChange={(id) =>
														onSave(assignNettingSet(pf, i.id, id))
													}
												/>
											) : under ? (
												`${under.counterparty.name}: ${under.nettingSet.name}`
											) : (
												"No counterparty"
											)}
										</td>
									</tr>
								);
							})}
						</tbody>
					</table>
				</section>
			)}
		</div>
	);
}
