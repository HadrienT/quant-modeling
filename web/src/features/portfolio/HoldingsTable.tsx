import { Fragment, useState } from "react";
import { ChevronRight } from "lucide-react";
import type { PositionMark } from "@/shared/api";
import { Badge, Button, DeltaBadge, NumberCell } from "@/shared/ui";
import { MarkInputs } from "./MarkInputs";
import { STATUS_TONE, worstStatus } from "./markStatus";

const HEAD = [
	"Instrument",
	"Qty",
	"Avg cost",
	"Mark",
	"Market value",
	"Unrealised",
	"Realised",
	"Day P&L",
	"Inputs",
	"",
];

/**
 * Open positions marked to market, and closed ones still carrying realised
 * P&L. Local-currency columns (average cost, mark) are per unit; the others
 * are totals in the base currency. A row opens to show every market input
 * of its mark, with where it came from.
 */
export function HoldingsTable({
	positions,
	base,
	onTrade,
}: {
	positions: PositionMark[];
	base: string;
	onTrade: (p: PositionMark, side: "buy" | "sell") => void;
}) {
	const [open, setOpen] = useState<string | null>(null);
	const [showClosed, setShowClosed] = useState(false);
	const closed = positions.filter((p) => p.quantity === 0).length;
	const rows = showClosed
		? positions
		: positions.filter((p) => p.quantity !== 0);

	return (
		<div className="flex flex-col gap-1.5">
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
				<table className="w-full text-xs">
					<thead>
						<tr className="text-2xs text-ink-muted uppercase">
							{HEAD.map((h, i) => (
								<th key={i} className="p-2 text-right first:text-left">
									{h}
									{[
										"Market value",
										"Unrealised",
										"Realised",
										"Day P&L",
									].includes(h) && ` (${base})`}
								</th>
							))}
						</tr>
					</thead>
					<tbody>
						{rows.map((p) => (
							<Fragment key={p.instrument_id}>
								<tr className="border-t border-hairline hover:bg-surface-raised">
									<td className="p-2 text-left text-ink">
										<button
											type="button"
											aria-expanded={open === p.instrument_id}
											className="inline-flex items-center gap-1 text-left"
											onClick={() =>
												setOpen(
													open === p.instrument_id ? null : p.instrument_id,
												)
											}
										>
											<ChevronRight
												className={`size-3 transition-transform ${open === p.instrument_id ? "rotate-90" : ""}`}
											/>
											{p.label}
											{p.quantity < 0 && <Badge tone="warning">short</Badge>}
										</button>
									</td>
									<td className="p-2 text-right">
										<NumberCell value={p.quantity} className="text-xs" />
									</td>
									<td className="p-2 text-right">
										<NumberCell
											value={p.quantity ? p.average_cost : null}
											className="text-xs"
										/>{" "}
										<span className="text-ink-muted">{p.currency}</span>
									</td>
									<td className="p-2 text-right">
										<NumberCell value={p.mark} className="text-xs" />
									</td>
									<td className="p-2 text-right">
										<NumberCell
											value={p.market_value_base}
											className="text-xs"
										/>
									</td>
									<td className="p-2 text-right">
										<DeltaBadge value={p.unrealised_base} className="text-xs" />
									</td>
									<td className="p-2 text-right">
										<DeltaBadge value={p.realised_base} className="text-xs" />
									</td>
									<td className="p-2 text-right">
										<DeltaBadge value={p.day_pnl_base} className="text-xs" />
									</td>
									<td className="p-2 text-right">
										{p.note && p.mark == null ? (
											<Badge tone="critical">no mark</Badge>
										) : (
											<Badge tone={STATUS_TONE[worstStatus(p.inputs)]}>
												{worstStatus(p.inputs)}
											</Badge>
										)}
									</td>
									<td className="p-2 text-right whitespace-nowrap">
										<Button
											size="sm"
											variant="ghost"
											onClick={() => onTrade(p, "buy")}
										>
											Buy
										</Button>
										<Button
											size="sm"
											variant="ghost"
											onClick={() => onTrade(p, "sell")}
										>
											Sell
										</Button>
									</td>
								</tr>
								{open === p.instrument_id && (
									<tr className="bg-surface-raised">
										<td colSpan={HEAD.length} className="p-3">
											<MarkInputs position={p} />
										</td>
									</tr>
								)}
							</Fragment>
						))}
					</tbody>
				</table>
			</div>
			{closed > 0 && (
				<button
					type="button"
					className="self-start text-2xs text-ink-muted hover:text-ink"
					onClick={() => setShowClosed((v) => !v)}
				>
					{showClosed ? "Hide" : "Show"} {closed} closed position(s)
				</button>
			)}
		</div>
	);
}
