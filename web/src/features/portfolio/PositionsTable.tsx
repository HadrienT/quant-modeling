import { useRef } from "react";
import { useVirtualizer } from "@tanstack/react-virtual";
import { Trash2 } from "lucide-react";
import type { Position } from "@/shared/api";
import { DeltaBadge, EngineTag, NumberCell } from "@/shared/ui";

/**
 * Virtualised positions table (WP 09 §1). 1000 rows stay fluid because only the
 * visible window is in the DOM. Pinned totals row at the bottom, always visible.
 */
export function PositionsTable({
	positions,
	onChange,
}: {
	positions: Position[];
	onChange: (next: Position[]) => void;
}) {
	const parentRef = useRef<HTMLDivElement>(null);
	const rows = useVirtualizer({
		count: positions.length,
		getScrollElement: () => parentRef.current,
		estimateSize: () => 34,
		overscan: 10,
	});

	const totals = positions.reduce(
		(a, p) => {
			const dir = p.direction === "short" ? -1 : 1;
			a.npv += p.result?.npv ?? 0;
			a.pnl += (p.result?.npv ?? 0) - p.entry_price * p.quantity * dir;
			return a;
		},
		{ npv: 0, pnl: 0 },
	);

	const cols = "grid grid-cols-[1fr_60px_70px_80px_90px_90px_90px_36px] gap-2";

	return (
		<div className="overflow-hidden rounded-md border border-hairline bg-surface">
			<div
				className={`${cols} border-b border-hairline px-3 py-1.5 text-2xs text-ink-muted uppercase`}
			>
				<span>Instrument</span>
				<span>Dir</span>
				<span className="text-right">Qty</span>
				<span className="text-right">Entry</span>
				<span className="text-right">NPV</span>
				<span className="text-right">P&L</span>
				<span className="text-right">Engine</span>
				<span />
			</div>

			<div ref={parentRef} className="max-h-[520px] overflow-auto">
				<div style={{ height: rows.getTotalSize(), position: "relative" }}>
					{rows.getVirtualItems().map((vr) => {
						const p = positions[vr.index]!;
						const dir = p.direction === "short" ? -1 : 1;
						const pnl = (p.result?.npv ?? 0) - p.entry_price * p.quantity * dir;
						return (
							<div
								key={p.id}
								className={`${cols} absolute top-0 left-0 w-full items-center px-3 text-xs hover:bg-surface-raised`}
								style={{ transform: `translateY(${vr.start}px)`, height: 34 }}
							>
								<span className="truncate text-ink" title={p.label}>
									{p.label}
								</span>
								<span className="text-ink-secondary">{p.direction}</span>
								<NumberCell value={p.quantity} magnitude="integer" />
								<NumberCell value={p.entry_price} magnitude="price" />
								<NumberCell value={p.result?.npv} magnitude="price" />
								<span className="text-right">
									<DeltaBadge value={p.result ? pnl : null} magnitude="price" />
								</span>
								<span className="text-right">
									{p.result?.engine ? (
										<EngineTag engine={p.result.engine} />
									) : (
										<span className="text-ink-muted">—</span>
									)}
								</span>
								<button
									type="button"
									aria-label={`Remove ${p.label}`}
									className="text-ink-muted hover:text-critical"
									onClick={() =>
										onChange(positions.filter((x) => x.id !== p.id))
									}
								>
									<Trash2 className="size-3.5" />
								</button>
							</div>
						);
					})}
				</div>
			</div>

			<div
				className={`${cols} border-t border-hairline px-3 py-1.5 text-xs font-medium`}
			>
				<span className="text-ink-secondary">Total ({positions.length})</span>
				<span />
				<span />
				<span />
				<NumberCell value={totals.npv} magnitude="price" />
				<span className="text-right">
					<DeltaBadge value={totals.pnl} magnitude="price" />
				</span>
				<span />
				<span />
			</div>
		</div>
	);
}
