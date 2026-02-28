import type { CategoryType, PricingResponse } from "./types";

interface Props {
	result: PricingResponse | null;
	loading: boolean;
	category: CategoryType;
	formatWithError: (v?: number | null, err?: number | null) => string;
}

function fmt(v?: number | null): string {
	if (v === null || v === undefined) return "—";
	return v.toFixed(6);
}

export default function ResultsPanel({ result, loading, category, formatWithError }: Props) {
	const isBond = category === "fixed-income";
	const ba = result?.bond_analytics;

	return (
		<section className="card">
			<h2>Results</h2>
			{!result && !loading && (
				<p className="price-muted">Run a pricing request to see results.</p>
			)}
			{result && (
				<div className="result-stack">
					<div className="result-kv">
						<span className="result-label">NPV</span>
						<span className="result-value">{formatWithError(result.npv, result.mc_std_error)}</span>
					</div>

					{/* ── Greeks (options / futures only) ─── */}
					{!isBond && (
						<div className="result-grid">
							<div>
								<span className="result-label">Delta</span>
								<span className="result-value">{formatWithError(result.greeks.delta, result.greeks.delta_std_error)}</span>
							</div>
							<div>
								<span className="result-label">Gamma</span>
								<span className="result-value">{formatWithError(result.greeks.gamma, result.greeks.gamma_std_error)}</span>
							</div>
							<div>
								<span className="result-label">Vega</span>
								<span className="result-value">{formatWithError(result.greeks.vega, result.greeks.vega_std_error)}</span>
							</div>
							<div>
								<span className="result-label">Theta</span>
								<span className="result-value">{formatWithError(result.greeks.theta, result.greeks.theta_std_error)}</span>
							</div>
							<div>
								<span className="result-label">Rho</span>
								<span className="result-value">{formatWithError(result.greeks.rho, result.greeks.rho_std_error)}</span>
							</div>
						</div>
					)}

					{/* ── Bond Analytics (fixed-income only) ─── */}
					{isBond && ba && (
						<div className="result-grid">
							<div>
								<span className="result-label">Macaulay Duration</span>
								<span className="result-value">{fmt(ba.macaulay_duration)}</span>
							</div>
							<div>
								<span className="result-label">Modified Duration</span>
								<span className="result-value">{fmt(ba.modified_duration)}</span>
							</div>
							<div>
								<span className="result-label">Convexity</span>
								<span className="result-value">{fmt(ba.convexity)}</span>
							</div>
							<div>
								<span className="result-label">DV01</span>
								<span className="result-value">{fmt(ba.dv01)}</span>
							</div>
						</div>
					)}
				</div>
			)}
		</section>
	);
}
