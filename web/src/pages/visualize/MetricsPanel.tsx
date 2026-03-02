type Props = {
	maxProfit: number;
	maxLoss: number;
	breakevens: number[];
	netPremium: number;
	hasLegs: boolean;
};

function fmt(v: number): string {
	if (!isFinite(v)) return "∞";
	return v.toFixed(2);
}

export default function MetricsPanel({
	maxProfit,
	maxLoss,
	breakevens,
	netPremium,
	hasLegs,
}: Props) {
	if (!hasLegs) return null;

	return (
		<div className="metrics-panel">
			<div className="metric">
				<span className="metric-label">Max Profit</span>
				<span className="metric-value profit">{fmt(maxProfit)}</span>
			</div>
			<div className="metric">
				<span className="metric-label">Max Loss</span>
				<span className="metric-value loss">{fmt(maxLoss)}</span>
			</div>
			<div className="metric">
				<span className="metric-label">
					Breakeven{breakevens.length !== 1 ? "s" : ""}
				</span>
				<span className="metric-value">
					{breakevens.length > 0
						? breakevens.map((b) => b.toFixed(2)).join(", ")
						: "—"}
				</span>
			</div>
			<div className="metric">
				<span className="metric-label">Net Premium</span>
				<span className="metric-value">
					{fmt(netPremium)}{" "}
					{netPremium > 0.005
						? "(debit)"
						: netPremium < -0.005
							? "(credit)"
							: ""}
				</span>
			</div>
		</div>
	);
}
