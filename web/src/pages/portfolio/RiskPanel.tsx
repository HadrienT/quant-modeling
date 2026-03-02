import type { PortfolioRiskSummary, StressResult, VaRResult } from "../../api/client";

type Props = {
	risk: PortfolioRiskSummary | null;
	var_: VaRResult | null;
	stress: StressResult[];
	onRunVar: () => void;
	onRunStress: () => void;
	loading: boolean;
};

function fmt(n: number | null | undefined, dp = 4): string {
	if (n == null) return "—";
	return n.toLocaleString(undefined, { minimumFractionDigits: dp, maximumFractionDigits: dp });
}

function fmtCurrency(n: number | null | undefined): string {
	if (n == null) return "—";
	return n.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
}

export default function RiskPanel({ risk, var_, stress, onRunVar, onRunStress, loading }: Props) {
	if (!risk) return null;

	return (
		<div className="risk-panel">
			<h3>Portfolio Risk</h3>

			{/* ── Aggregated Greeks ── */}
			<div className="risk-greeks">
				<div className="risk-metric">
					<span className="risk-label">NPV</span>
					<span className={`risk-value ${risk.total_npv >= 0 ? "profit" : "loss"}`}>
						{fmtCurrency(risk.total_npv)}
					</span>
				</div>
				<div className="risk-metric">
					<span className="risk-label">P&L</span>
					<span className={`risk-value ${risk.total_pnl >= 0 ? "profit" : "loss"}`}>
						{fmtCurrency(risk.total_pnl)}
					</span>
				</div>
				<div className="risk-metric">
					<span className="risk-label">Delta</span>
					<span className="risk-value">{fmt(risk.total_delta)}</span>
				</div>
				<div className="risk-metric">
					<span className="risk-label">Gamma</span>
					<span className="risk-value">{fmt(risk.total_gamma)}</span>
				</div>
				<div className="risk-metric">
					<span className="risk-label">Vega</span>
					<span className="risk-value">{fmt(risk.total_vega)}</span>
				</div>
				<div className="risk-metric">
					<span className="risk-label">Theta</span>
					<span className="risk-value">{fmt(risk.total_theta)}</span>
				</div>
				<div className="risk-metric">
					<span className="risk-label">Rho</span>
					<span className="risk-value">{fmt(risk.total_rho)}</span>
				</div>
				<div className="risk-metric">
					<span className="risk-label">Priced</span>
					<span className="risk-value">{risk.positions_priced}/{risk.positions_total}</span>
				</div>
			</div>

			{/* ── VaR ── */}
			<div className="risk-section">
				<div className="risk-section-header">
					<h4>Value at Risk</h4>
					<button type="button" className="btn-sm" onClick={onRunVar} disabled={loading}>
						{loading ? "…" : "Compute"}
					</button>
				</div>
				{var_ && (
					<div className="risk-greeks">
						<div className="risk-metric">
							<span className="risk-label">VaR ({(var_.confidence * 100).toFixed(0)}%)</span>
							<span className="risk-value loss">{fmtCurrency(var_.var)}</span>
						</div>
						<div className="risk-metric">
							<span className="risk-label">ES</span>
							<span className="risk-value loss">{fmtCurrency(var_.expected_shortfall)}</span>
						</div>
						<div className="risk-metric">
							<span className="risk-label">Horizon</span>
							<span className="risk-value">{var_.horizon_days}d</span>
						</div>
						<div className="risk-metric">
							<span className="risk-label">Method</span>
							<span className="risk-value">{var_.method}</span>
						</div>
					</div>
				)}
			</div>

			{/* ── Stress tests ── */}
			<div className="risk-section">
				<div className="risk-section-header">
					<h4>Stress Tests</h4>
					<button type="button" className="btn-sm" onClick={onRunStress} disabled={loading}>
						{loading ? "…" : "Run"}
					</button>
				</div>
				{stress.length > 0 && (
					<table className="stress-table">
						<thead>
							<tr>
								<th>Scenario</th>
								<th>Portfolio P&L</th>
							</tr>
						</thead>
						<tbody>
							{stress.map((s) => (
								<tr key={s.name}>
									<td>{s.name}</td>
									<td className={s.portfolio_pnl >= 0 ? "profit" : "loss"}>
										{fmtCurrency(s.portfolio_pnl)}
									</td>
								</tr>
							))}
						</tbody>
					</table>
				)}
			</div>
		</div>
	);
}
