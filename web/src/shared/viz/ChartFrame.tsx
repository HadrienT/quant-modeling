import { type ReactNode, useId, useMemo, useState } from "react";
import { Download, Table2 } from "lucide-react";
import { Button, cn } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { ChartSkeleton } from "@/shared/ui/states";

/**
 * Every chart is wrapped in this (blueprint WP 06 §3), which guarantees that
 * loading / empty / error states, the legend, CSV + PNG export and the
 * accessibility table view exist everywhere without each chart re-implementing them.
 */

export type LegendItem = { label: string; color: string };

export type TableView = {
	columns: string[];
	rows: (string | number)[][];
};

export function ChartFrame({
	title,
	subtitle,
	legend,
	table,
	isLoading,
	error,
	onRetry,
	isEmpty,
	emptyLabel = "No data for this selection.",
	height = 300,
	children,
	actions,
}: {
	title?: string;
	subtitle?: string;
	legend?: LegendItem[];
	table?: TableView;
	isLoading?: boolean;
	error?: unknown;
	onRetry?: () => void;
	isEmpty?: boolean;
	emptyLabel?: string;
	height?: number;
	children: ReactNode;
	actions?: ReactNode;
}) {
	const [showTable, setShowTable] = useState(false);
	const headingId = useId();

	const csv = useMemo(() => {
		if (!table) return "";
		const esc = (c: string | number) =>
			typeof c === "string" && /[",\n]/.test(c)
				? `"${c.replace(/"/g, '""')}"`
				: c;
		return [table.columns, ...table.rows]
			.map((r) => r.map(esc).join(","))
			.join("\n");
	}, [table]);

	function downloadCsv() {
		const blob = new Blob([csv], { type: "text/csv" });
		const url = URL.createObjectURL(blob);
		const a = document.createElement("a");
		a.href = url;
		a.download = `${title ?? "chart"}.csv`;
		a.click();
		URL.revokeObjectURL(url);
	}

	return (
		<figure
			className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-3"
			aria-labelledby={title ? headingId : undefined}
		>
			<div className="flex items-start justify-between gap-3">
				<div className="flex flex-col">
					{title && (
						<figcaption id={headingId} className="text-sm font-medium text-ink">
							{title}
						</figcaption>
					)}
					{subtitle && (
						<span className="text-2xs text-ink-muted">{subtitle}</span>
					)}
				</div>
				<div className="flex items-center gap-1">
					{actions}
					{table && (
						<>
							<Button
								variant="ghost"
								size="icon"
								aria-pressed={showTable}
								aria-label="Toggle data table"
								onClick={() => setShowTable((v) => !v)}
							>
								<Table2 className="size-3.5" />
							</Button>
							<Button
								variant="ghost"
								size="icon"
								aria-label="Download CSV"
								onClick={downloadCsv}
							>
								<Download className="size-3.5" />
							</Button>
						</>
					)}
				</div>
			</div>

			{legend && legend.length > 1 && (
				<ul className="flex flex-wrap gap-x-4 gap-y-1">
					{legend.map((l) => (
						<li
							key={l.label}
							className="flex items-center gap-1.5 text-2xs text-ink-secondary"
						>
							<span
								className="size-2 rounded-[2px]"
								style={{ background: l.color }}
								aria-hidden="true"
							/>
							{l.label}
						</li>
					))}
				</ul>
			)}

			<div style={{ minHeight: height }} className="relative">
				{error ? (
					<ErrorState error={error} onRetry={onRetry} compact />
				) : isLoading ? (
					<ChartSkeleton className="!aspect-auto" />
				) : isEmpty ? (
					<div className="flex h-full items-center justify-center py-10 text-xs text-ink-muted">
						{emptyLabel}
					</div>
				) : (
					<>
						{/*
						 * Keep the chart mounted and sized even when the table is shown,
						 * so canvas engines (lightweight-charts) don't lose their instance
						 * and come back blank on toggle-off. The table sits on top.
						 */}
						{children}
						{showTable && table && (
							<div className="absolute inset-0 overflow-auto bg-surface">
								<DataTable table={table} />
							</div>
						)}
					</>
				)}
			</div>
		</figure>
	);
}

function DataTable({ table }: { table: TableView }) {
	return (
		<div className="max-h-72 overflow-auto">
			<table className="w-full text-xs">
				<thead className="sticky top-0 bg-surface">
					<tr>
						{table.columns.map((c) => (
							<th
								key={c}
								className={cn(
									"border-b border-hairline px-2 py-1 text-right font-medium text-ink-secondary first:text-left",
								)}
							>
								{c}
							</th>
						))}
					</tr>
				</thead>
				<tbody className="font-mono tabular-nums">
					{table.rows.map((row, i) => (
						<tr key={i} className="hover:bg-surface-raised">
							{row.map((cell, j) => (
								<td
									key={j}
									className="px-2 py-0.5 text-right text-ink first:text-left"
								>
									{cell}
								</td>
							))}
						</tr>
					))}
				</tbody>
			</table>
		</div>
	);
}
