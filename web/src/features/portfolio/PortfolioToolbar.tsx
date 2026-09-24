import { Download, Upload } from "lucide-react";
import type { Portfolio, PortfolioCurrency } from "@/shared/api";
import { CURRENCIES } from "@/shared/portfolio";
import { Button, Input } from "@/shared/ui";

function download(name: string, blob: Blob) {
	const a = document.createElement("a");
	a.href = URL.createObjectURL(blob);
	a.download = name;
	a.click();
}

/** The journal as CSV: one row per trade, signed quantities. */
function journalCsv(pf: Portfolio): string {
	const label = new Map((pf.instruments ?? []).map((i) => [i.id, i.label]));
	const rows = [
		["trade_date", "instrument", "quantity", "price", "fees", "note"],
		...(pf.transactions ?? []).map((t) => [
			t.trade_date,
			JSON.stringify(label.get(t.instrument_id) ?? t.instrument_id),
			t.quantity,
			t.price,
			t.fees,
			JSON.stringify(t.note ?? ""),
		]),
	];
	return rows.map((r) => r.join(",")).join("\n");
}

/** Base currency, export (JSON, journal CSV) and import. */
export function PortfolioToolbar({
	pf,
	onBaseCurrency,
	onImport,
}: {
	pf: Portfolio | null | undefined;
	onBaseCurrency: (c: PortfolioCurrency) => void;
	onImport: (file: File) => void;
}) {
	const box =
		"h-8 rounded-sm border border-hairline bg-surface px-2 text-xs text-ink";
	return (
		<div className="flex flex-wrap items-center gap-1.5">
			{pf && (
				<select
					aria-label="Base currency"
					title="Currency the portfolio is reported in (ECB rates)"
					className={box}
					value={pf.base_currency}
					onChange={(e) => onBaseCurrency(e.target.value as PortfolioCurrency)}
				>
					{CURRENCIES.map((c) => (
						<option key={c}>{c}</option>
					))}
				</select>
			)}
			<Button
				size="sm"
				variant="ghost"
				disabled={!pf}
				onClick={() =>
					pf &&
					download(
						`${pf.name}.json`,
						new Blob([JSON.stringify(pf, null, 2)], {
							type: "application/json",
						}),
					)
				}
			>
				<Download className="size-3.5" /> JSON
			</Button>
			<Button
				size="sm"
				variant="ghost"
				disabled={!pf}
				onClick={() =>
					pf &&
					download(
						`${pf.name}-journal.csv`,
						new Blob([journalCsv(pf)], { type: "text/csv" }),
					)
				}
			>
				<Download className="size-3.5" /> CSV
			</Button>
			<label className="inline-flex cursor-pointer items-center gap-1 rounded-sm px-2 py-1 text-xs text-ink-secondary hover:text-ink">
				<Upload className="size-3.5" /> Import
				<Input
					type="file"
					accept="application/json"
					className="hidden"
					onChange={(e) => e.target.files?.[0] && onImport(e.target.files[0])}
				/>
			</label>
		</div>
	);
}
