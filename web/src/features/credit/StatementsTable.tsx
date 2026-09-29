import type {
	FigureCell,
	FundamentalsResponse,
	StatementRow,
} from "@/shared/api";
import { cn } from "@/shared/ui";
import { dash, figure } from "./format";

const SECTIONS: { key: StatementRow["statement"]; title: string }[] = [
	{ key: "income", title: "Income statement" },
	{ key: "balance", title: "Balance sheet" },
	{ key: "cash_flow", title: "Cash flow statement" },
	{ key: "ratio", title: "Ratios" },
];

/** Where a figure comes from, for the cell's tooltip. */
function sourceText(cell: FigureCell): string {
	const lines = cell.sources.map(
		(s) => `${s.concept} — ${s.accession}, filed ${s.filed}`,
	);
	return cell.formula ? [cell.formula, ...lines].join("\n") : lines.join("\n");
}

function Cell({ row, cell }: { row: StatementRow; cell: FigureCell | null }) {
	if (!cell) return <span className="text-ink-muted">{dash}</span>;
	const text = figure(cell.value, row.unit, row.key);
	const url = cell.sources.length === 1 ? cell.sources[0]!.url : null;
	return url ? (
		<a
			href={url}
			target="_blank"
			rel="noreferrer"
			title={sourceText(cell)}
			className="decoration-hairline hover:underline"
		>
			{text}
		</a>
	) : (
		<span title={sourceText(cell)}>{text}</span>
	);
}

/**
 * The statements as a dense table, one column per period. Hover a figure
 * for the XBRL concept and filing it comes from (and the formula of a
 * derived line); click it to open the filing.
 */
export function StatementsTable({ data }: { data: FundamentalsResponse }) {
	const rows = [...data.rows, ...data.ratios];
	return (
		<div className="overflow-x-auto rounded-sm border border-hairline">
			<table className="w-full text-sm">
				<thead className="bg-surface-raised text-xs text-ink-secondary">
					<tr>
						<th className="px-3 py-2 text-left font-medium">
							{data.frequency === "annual"
								? "Fiscal year ending"
								: "Quarter ending"}
						</th>
						{data.periods.map((p) => (
							<th
								key={p}
								className="px-3 py-2 text-right font-medium tabular-nums"
							>
								{p}
							</th>
						))}
					</tr>
				</thead>
				{SECTIONS.map((s) => (
					<tbody key={s.key}>
						<tr className="border-t border-hairline bg-surface">
							<th
								colSpan={data.periods.length + 1}
								className="px-3 py-1.5 text-left text-2xs font-semibold tracking-wide text-ink-secondary uppercase"
							>
								{s.title}
							</th>
						</tr>
						{rows
							.filter((r) => r.statement === s.key)
							.map((r) => (
								<tr key={r.key} className="border-t border-hairline/60">
									<td
										className={cn(
											"px-3 py-1.5 text-ink",
											r.derived && "text-ink-secondary italic",
										)}
										title={r.derived ? (r.formula ?? undefined) : undefined}
									>
										{r.label}
									</td>
									{r.values.map((c, i) => (
										<td key={i} className="px-3 py-1.5 text-right tabular-nums">
											<Cell row={r} cell={c} />
										</td>
									))}
								</tr>
							))}
					</tbody>
				))}
			</table>
		</div>
	);
}
