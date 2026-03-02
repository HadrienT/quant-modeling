import type { Position } from "../../api/client";
import { findProduct } from "./catalog";

type Props = {
	positions: Position[];
	onRemove: (id: string) => void;
	onSelect?: (pos: Position) => void;
};

function fmt(n: number | null | undefined, dp = 4): string {
	if (n == null) return "—";
	return n.toLocaleString(undefined, { minimumFractionDigits: dp, maximumFractionDigits: dp });
}

function fmtCurrency(n: number | null | undefined): string {
	if (n == null) return "—";
	return n.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
}

export default function PositionTable({ positions, onRemove, onSelect }: Props) {
	if (positions.length === 0) {
		return <p className="empty-msg">No positions yet. Click "Add Position" to get started.</p>;
	}

	return (
		<div className="position-table-wrap">
			<table className="position-table">
				<thead>
					<tr>
						<th>Label</th>
						<th>Product</th>
						<th>Engine</th>
						<th>Dir</th>
						<th>Qty</th>
						<th>Entry</th>
						<th>NPV</th>
						<th>Unit</th>
						<th>Δ</th>
						<th>Γ</th>
						<th>ν</th>
						<th>θ</th>
						<th>ρ</th>
						<th></th>
					</tr>
				</thead>
				<tbody>
					{positions.map((pos) => {
						const def = findProduct(pos.product_type as never);
						const r = pos.result;
						const npvClass = r && r.npv !== 0 ? (r.npv >= 0 ? "profit" : "loss") : "";
						return (
							<tr key={pos.id} className="pos-row-clickable" onClick={() => onSelect?.(pos)}>
								<td className="pos-label">{pos.label}</td>
								<td>{def?.label ?? pos.product_type}</td>
								<td className="pos-engine">{r?.engine || "—"}</td>
								<td>
									<span className={`dir-badge ${pos.direction}`}>{pos.direction}</span>
								</td>
								<td>{pos.quantity}</td>
								<td>{fmtCurrency(pos.entry_price)}</td>
								<td className={npvClass}>{r ? fmtCurrency(r.npv) : "—"}</td>
								<td>{r ? fmtCurrency(r.unit_price) : "—"}</td>
								<td>{r ? fmt(r.greeks.delta) : "—"}</td>
								<td>{r ? fmt(r.greeks.gamma) : "—"}</td>
								<td>{r ? fmt(r.greeks.vega) : "—"}</td>
								<td>{r ? fmt(r.greeks.theta) : "—"}</td>
								<td>{r ? fmt(r.greeks.rho) : "—"}</td>
								<td>
									<button className="btn-icon btn-remove" type="button" onClick={(e) => { e.stopPropagation(); onRemove(pos.id); }}>
										✕
									</button>
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>
		</div>
	);
}
