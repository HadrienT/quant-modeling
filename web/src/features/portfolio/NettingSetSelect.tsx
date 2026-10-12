import type { Counterparty } from "@/shared/api";

/**
 * The netting set a derivative is traded under, among the portfolio's
 * counterparties' — or none: its counterparty risk is then not looked at.
 */
export function NettingSetSelect({
	counterparties,
	value,
	onChange,
	label,
}: {
	counterparties: Counterparty[];
	value: string | null;
	onChange: (nettingSetId: string | null) => void;
	label: string;
}) {
	return (
		<select
			aria-label={label}
			className="h-9 max-w-full rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
			value={value ?? ""}
			onChange={(e) => onChange(e.target.value || null)}
		>
			<option value="">No counterparty</option>
			{counterparties.map((c) => (
				<optgroup key={c.id} label={`${c.name} (${c.rating})`}>
					{(c.netting_sets ?? []).map((n) => (
						<option key={n.id} value={n.id}>
							{n.name}
						</option>
					))}
				</optgroup>
			))}
		</select>
	);
}
