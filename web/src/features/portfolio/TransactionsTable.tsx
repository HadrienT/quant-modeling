import { useState } from "react";
import { Trash2 } from "lucide-react";
import type { Instrument, Trade } from "@/shared/api";
import { Badge, Button, DeltaBadge, NumberCell } from "@/shared/ui";

const HEAD = [
	"Date",
	"Side",
	"Instrument",
	"Qty",
	"Price",
	"Fees",
	"Cash flow",
	"Note",
	"",
];

/**
 * The portfolio's journal, newest first. Positions are derived from it: a
 * correction is a new trade, or deleting the wrong one (confirmed, since
 * it rewrites the history of the P&L). Cash flow follows the cash-flow
 * convention — paid is negative — in the instrument's currency.
 */
export function TransactionsTable({
	trades,
	instruments,
	currencies,
	onDelete,
	readOnly = false,
}: {
	trades: Trade[];
	instruments: Instrument[];
	/** instrument id → currency, from the valuation (a stock's is the listing's) */
	currencies: Record<string, string>;
	onDelete: (id: string) => void;
	readOnly?: boolean;
}) {
	const [confirming, setConfirming] = useState<string | null>(null);
	const byId = new Map(instruments.map((i) => [i.id, i]));
	const rows = [...trades].sort(
		(a, b) =>
			b.trade_date.localeCompare(a.trade_date) ||
			(b.created_at ?? "").localeCompare(a.created_at ?? ""),
	);

	if (!rows.length) {
		return <p className="text-sm text-ink-muted">No transactions yet.</p>;
	}

	return (
		<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
			<table className="w-full text-xs">
				<thead>
					<tr className="text-2xs text-ink-muted uppercase">
						{HEAD.map((h, i) => (
							<th
								key={i}
								className="p-2 text-right [&:nth-child(-n+3)]:text-left"
							>
								{h}
							</th>
						))}
					</tr>
				</thead>
				<tbody>
					{rows.map((t) => {
						const inst = byId.get(t.instrument_id);
						const ccy =
							currencies[t.instrument_id] ??
							(inst?.spec.kind === "derivative" ? inst.spec.currency : "");
						return (
							<tr key={t.id} className="border-t border-hairline">
								<td className="p-2 font-mono text-ink-secondary">
									{t.trade_date}
								</td>
								<td className="p-2">
									<Badge tone={t.quantity > 0 ? "good" : "critical"}>
										{t.quantity > 0 ? "buy" : "sell"}
									</Badge>
								</td>
								<td className="p-2 text-ink">
									{inst?.label ?? t.instrument_id}
								</td>
								<td className="p-2 text-right">
									<NumberCell
										value={Math.abs(t.quantity)}
										className="text-xs"
									/>
								</td>
								<td className="p-2 text-right">
									<NumberCell value={t.price} className="text-xs" />
								</td>
								<td className="p-2 text-right">
									<NumberCell value={t.fees} className="text-xs" />
								</td>
								<td className="p-2 text-right">
									<DeltaBadge
										value={-(t.quantity * t.price + t.fees)}
										className="text-xs"
									/>{" "}
									<span className="text-ink-muted">{ccy}</span>
								</td>
								<td
									className="max-w-48 truncate p-2 text-ink-muted"
									title={t.note}
								>
									{t.note}
								</td>
								<td className="p-2 text-right whitespace-nowrap">
									{readOnly ? null : confirming === t.id ? (
										<>
											<Button
												size="sm"
												variant="danger"
												onClick={() => onDelete(t.id)}
											>
												Delete
											</Button>
											<Button
												size="sm"
												variant="ghost"
												onClick={() => setConfirming(null)}
											>
												Keep
											</Button>
										</>
									) : (
										<button
											type="button"
											aria-label={`Delete trade of ${t.trade_date}`}
											className="text-ink-muted hover:text-critical"
											onClick={() => setConfirming(t.id)}
										>
											<Trash2 className="size-3.5" />
										</button>
									)}
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>
		</div>
	);
}
