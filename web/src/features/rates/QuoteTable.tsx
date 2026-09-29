import { Button } from "@/shared/ui";
import { SCALE, STEP, SUFFIX, type Unit } from "./format";

export type Column<R> = { key: keyof R & string; label: string; unit: Unit };

/**
 * An editable table of quotes: one numeric input per cell, shown in the
 * column's unit (% for rates, bp for vols, years for times) and stored as a
 * decimal. Rows can be added (a copy of the last) and removed.
 */
export function QuoteTable<R extends Record<string, number>>({
	title,
	columns,
	rows,
	onChange,
	minRows = 1,
}: {
	title: string;
	columns: Column<R>[];
	rows: R[];
	onChange: (rows: R[]) => void;
	minRows?: number;
}) {
	const set = (i: number, key: keyof R, raw: string, unit: Unit) => {
		const v = Number.parseFloat(raw);
		if (!Number.isFinite(v)) return;
		onChange(
			rows.map((r, j) => (j === i ? { ...r, [key]: v / SCALE[unit] } : r)),
		);
	};
	return (
		<fieldset className="flex flex-col gap-1">
			<legend className="mb-1 text-xs font-semibold text-ink-secondary">
				{title}
			</legend>
			<table className="w-full text-xs">
				<thead>
					<tr className="text-ink-muted">
						{columns.map((c) => (
							<th key={c.key} className="px-1 text-left font-normal">
								{c.label} ({SUFFIX[c.unit]})
							</th>
						))}
						<th />
					</tr>
				</thead>
				<tbody>
					{rows.map((r, i) => (
						<tr key={i}>
							{columns.map((c) => (
								<td key={c.key} className="px-1 py-0.5">
									<input
										type="number"
										aria-label={`${title} row ${i + 1} ${c.label}`}
										step={STEP[c.unit]}
										className="w-full rounded-sm border border-hairline bg-canvas px-1 py-0.5 font-mono tabular-nums"
										value={+(r[c.key]! * SCALE[c.unit]).toPrecision(10)}
										onChange={(e) => set(i, c.key, e.target.value, c.unit)}
									/>
								</td>
							))}
							<td className="w-6 text-right">
								<button
									type="button"
									aria-label={`Remove ${title} row ${i + 1}`}
									disabled={rows.length <= minRows}
									onClick={() => onChange(rows.filter((_, j) => j !== i))}
									className="text-ink-muted hover:text-ink disabled:opacity-30"
								>
									×
								</button>
							</td>
						</tr>
					))}
				</tbody>
			</table>
			<Button
				variant="ghost"
				size="sm"
				className="self-start"
				disabled={rows.length === 0}
				onClick={() => onChange([...rows, { ...rows[rows.length - 1]! }])}
			>
				Add row
			</Button>
		</fieldset>
	);
}
