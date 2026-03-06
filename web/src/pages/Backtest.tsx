import { useState } from "react";
import Plot from "react-plotly.js";
import {
	type AllocationRow,
	type BacktestResponse,
	type PortfolioPoint,
	runBacktest,
} from "../api/client";

// ── Curated ticker lists (mirroring the old Streamlit app) ───────────────────

const TICKER_CATEGORIES: Record<string, string[]> = {
	Stocks: ["AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "BRK-B", "NVDA", "META", "JPM", "V", "UNH", "JNJ"],
	ETFs: ["SPY", "QQQ", "VTI", "EEM", "IWM", "GLD", "TLT", "XLF", "XLE", "ARKK"],
	Indices: ["^GSPC", "^DJI", "^IXIC", "^FTSE", "^GDAXI", "^FCHI", "^N225", "URTH"],
	Commodities: ["GC=F", "SI=F", "CL=F", "BZ=F", "HG=F"],
	Crypto: ["BTC-USD", "ETH-USD", "BNB-USD", "XRP-USD"],
	Bonds: ["^TYX"],
};

const EXAMPLE_PORTFOLIOS = [
	{
		label: "US Tech",
		tickers: ["AAPL", "MSFT", "GOOGL", "AMZN", "TSLA"],
		opt_start: "2023-01-01",
		opt_end: "2024-01-01",
		max_share: 0.4,
	},
	{
		label: "Diversified",
		tickers: ["SPY", "QQQ", "GLD", "TLT", "BTC-USD"],
		opt_start: "2021-06-01",
		opt_end: "2023-01-01",
		max_share: 0.5,
	},
	{
		label: "Growth + Crypto",
		tickers: ["NVDA", "META", "ETH-USD", "BNB-USD", "GC=F"],
		opt_start: "2023-12-01",
		opt_end: "2024-09-01",
		max_share: 0.6,
	},
	{
		label: "Defensive",
		tickers: ["BRK-B", "^DJI", "TLT", "GLD", "HG=F"],
		opt_start: "2021-01-01",
		opt_end: "2022-01-01",
		max_share: 0.3,
	},
];

// ── Chart helpers ─────────────────────────────────────────────────────────────

const CHART_LAYOUT_BASE = {
	paper_bgcolor: "transparent",
	plot_bgcolor: "rgba(255,255,255,0.7)",
	font: { family: "'Space Grotesk', system-ui, sans-serif", color: "#2b3238" },
	margin: { t: 32, r: 20, b: 48, l: 60 },
	xaxis: { gridcolor: "rgba(16,20,24,0.07)", zerolinecolor: "rgba(16,20,24,0.15)" },
	yaxis: { gridcolor: "rgba(16,20,24,0.07)", zerolinecolor: "rgba(16,20,24,0.15)" },
	legend: { bgcolor: "rgba(255,255,255,0.6)", bordercolor: "rgba(16,20,24,0.12)", borderwidth: 1 },
};

function PortfolioChart({
	portfolioValues,
	sp500Values,
	initialCapital,
}: {
	portfolioValues: PortfolioPoint[];
	sp500Values: PortfolioPoint[] | null;
	initialCapital: number;
}) {
	const traces: Plotly.Data[] = [
		{
			x: portfolioValues.map((p) => p.date),
			y: portfolioValues.map((p) => p.value),
			type: "scatter",
			mode: "lines",
			name: "Portfolio",
			line: { color: "#1f8a70", width: 2 },
			fill: "tozeroy",
			fillcolor: "rgba(31,138,112,0.07)",
		},
	];

	if (sp500Values && sp500Values.length > 0) {
		traces.push({
			x: sp500Values.map((p) => p.date),
			y: sp500Values.map((p) => p.value),
			type: "scatter",
			mode: "lines",
			name: "S&P 500 (rebased)",
			line: { color: "#ff8c42", width: 2, dash: "dot" },
		});
	}

	const layout: Partial<Plotly.Layout> = {
		...CHART_LAYOUT_BASE,
		title: { text: "Portfolio Value Over Time", font: { size: 15, color: "#101418" } },
		xaxis: { ...CHART_LAYOUT_BASE.xaxis, title: "Date" },
		yaxis: {
			...CHART_LAYOUT_BASE.yaxis,
			title: "Value ($)",
			tickformat: ",.0f",
		},
		shapes: [
			{
				type: "line",
				x0: portfolioValues[0]?.date,
				x1: portfolioValues[portfolioValues.length - 1]?.date,
				y0: initialCapital,
				y1: initialCapital,
				line: { color: "rgba(16,20,24,0.25)", width: 1, dash: "dash" },
			},
		],
	};

	return (
		<Plot
			data={traces}
			layout={layout}
			config={{ responsive: true, displayModeBar: false }}
			style={{ width: "100%", height: 340 }}
			useResizeHandler
		/>
	);
}

function AllocationChart({ allocation }: { allocation: AllocationRow[] }) {
	const sorted = [...allocation].sort((a, b) => b.weight - a.weight);
	const traces: Plotly.Data[] = [
		{
			x: sorted.map((r) => r.ticker),
			y: sorted.map((r) => +(r.weight * 100).toFixed(2)),
			type: "bar",
			marker: {
				color: sorted.map((_, i) =>
					i === 0 ? "#1f8a70" : i === 1 ? "#ff8c42" : i === 2 ? "#2f4858" : `hsl(${160 + i * 40}, 55%, 45%)`
				),
			},
			text: sorted.map((r) => `${(r.weight * 100).toFixed(1)}%`),
			textposition: "outside",
		},
	];

	const layout: Partial<Plotly.Layout> = {
		...CHART_LAYOUT_BASE,
		title: { text: "Optimal Allocation", font: { size: 15, color: "#101418" } },
		xaxis: { ...CHART_LAYOUT_BASE.xaxis, title: "Ticker" },
		yaxis: { ...CHART_LAYOUT_BASE.yaxis, title: "Weight (%)", range: [0, 100] },
		showlegend: false,
	};

	return (
		<Plot
			data={traces}
			layout={layout}
			config={{ responsive: true, displayModeBar: false }}
			style={{ width: "100%", height: 300 }}
			useResizeHandler
		/>
	);
}

// ── Metric display helpers ─────────────────────────────────────────────────────

function MetricCard({
	label,
	value,
	positive,
}: {
	label: string;
	value: string;
	positive?: boolean;
}) {
	const colour =
		positive === undefined
			? "var(--ink-0)"
			: positive
			? "#1f8a70"
			: "#b42318";
	return (
		<div
			style={{
				display: "flex",
				flexDirection: "column",
				gap: 4,
				padding: "12px 14px",
				borderRadius: 12,
				border: "1px solid var(--stroke)",
				background: "#fff",
			}}
		>
			<span className="result-label">{label}</span>
			<span className="result-value mono" style={{ color: colour }}>
				{value}
			</span>
		</div>
	);
}

// ── Main page ─────────────────────────────────────────────────────────────────

export default function BacktestPage() {
	// ── Form state ───────────────────────────────────────────────────────────
	const [selectedTickers, setSelectedTickers] = useState<string[]>([]);
	const [activeCategory, setActiveCategory] = useState<string>("Stocks");
	const [manualTicker, setManualTicker] = useState("");
	const [optStart, setOptStart] = useState("2023-01-01");
	const [optEnd, setOptEnd] = useState("2024-01-01");
	const [initialCapital, setInitialCapital] = useState(10000);
	const [maxShare, setMaxShare] = useState(0.4);
	const [minShare, setMinShare] = useState(0.0);
	const [rebalanceFreq, setRebalanceFreq] = useState(0);

	// ── Result state ─────────────────────────────────────────────────────────
	const [result, setResult] = useState<BacktestResponse | null>(null);
	const [loading, setLoading] = useState(false);
	const [error, setError] = useState<string | null>(null);

	// ── Ticker selection helpers ──────────────────────────────────────────────
	const toggleTicker = (ticker: string) => {
		setSelectedTickers((prev) =>
			prev.includes(ticker) ? prev.filter((t) => t !== ticker) : [...prev, ticker]
		);
	};

	const addManualTicker = () => {
		const t = manualTicker.trim().toUpperCase();
		if (t && !selectedTickers.includes(t)) {
			setSelectedTickers((prev) => [...prev, t]);
		}
		setManualTicker("");
	};

	const applyExample = (ex: (typeof EXAMPLE_PORTFOLIOS)[number]) => {
		setSelectedTickers(ex.tickers);
		setOptStart(ex.opt_start);
		setOptEnd(ex.opt_end);
		setMaxShare(ex.max_share);
	};

	// ── Submit ────────────────────────────────────────────────────────────────
	const handleRun = async () => {
		if (selectedTickers.length === 0) {
			setError("Please select at least one ticker.");
			return;
		}
		setError(null);
		setLoading(true);
		setResult(null);
		try {
			const res = await runBacktest({
				tickers: selectedTickers,
				opt_start: optStart,
				opt_end: optEnd,
				initial_capital: initialCapital,
				max_share: maxShare,
				min_share: minShare,
				rebalance_freq: rebalanceFreq,
			});
			setResult(res);
		} catch (e) {
			setError(e instanceof Error ? e.message : "Backtest failed");
		} finally {
			setLoading(false);
		}
	};

	// ── Derived result values ─────────────────────────────────────────────────
	const m = result?.metrics;
	const lastValue = result?.portfolio_values[result.portfolio_values.length - 1]?.value ?? 0;
	const returnPositive = m ? m.total_return_pct >= 0 : undefined;
	const ddPositive = m ? m.max_drawdown >= -0.05 : undefined;
	const sharpePositive = m ? m.sharpe_ratio >= 1 : undefined;

	return (
		<div className="page">
			{/* ── Hero ──────────────────────────────────────────────────────── */}
			<div className="hero">
				<div>
					<h1>Portfolio Backtest</h1>
					<p>
						Select assets, define an optimisation window, and see how a
						Sharpe-optimised portfolio would have performed.
					</p>
				</div>
				<div className="hero-card">
					<div className="result-label">Methodology</div>
					<p style={{ fontSize: "0.85rem", color: "var(--ink-1)", marginTop: 6, lineHeight: 1.5 }}>
						Historical returns on the look-back window are used to maximise the
						Sharpe ratio (SLSQP). The portfolio is then tracked forward from the
						investment start date, with optional periodic rebalancing.
					</p>
				</div>
			</div>

			{/* ── Main layout ───────────────────────────────────────────────── */}
			<div style={{ display: "grid", gridTemplateColumns: "minmax(320px, 420px) 1fr", gap: 24, alignItems: "start" }}>
				{/* ── Left panel: form ────────────────────────────────────── */}
				<div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
					{/* Example presets */}
					<div className="card" style={{ padding: "18px 20px" }}>
						<h3>Example Portfolios</h3>
						<div style={{ display: "flex", flexWrap: "wrap", gap: 8, marginTop: 10 }}>
							{EXAMPLE_PORTFOLIOS.map((ex) => (
								<button
									key={ex.label}
									type="button"
									className="pill"
									onClick={() => applyExample(ex)}
								>
									{ex.label}
								</button>
							))}
						</div>
					</div>

					{/* Ticker selection */}
					<div className="card" style={{ padding: "18px 20px" }}>
						<h3>Select Tickers</h3>

						{/* Category tabs */}
						<div className="category-bar" style={{ marginTop: 10 }}>
							{Object.keys(TICKER_CATEGORIES).map((cat) => (
								<button
									key={cat}
									type="button"
									className={`pill${activeCategory === cat ? " active" : ""}`}
									onClick={() => setActiveCategory(cat)}
								>
									{cat}
								</button>
							))}
						</div>

						{/* Ticker pills for active category */}
						<div style={{ display: "flex", flexWrap: "wrap", gap: 6, maxHeight: 160, overflowY: "auto" }}>
							{TICKER_CATEGORIES[activeCategory].map((ticker) => (
								<button
									key={ticker}
									type="button"
									className={`pill${selectedTickers.includes(ticker) ? " active" : ""}`}
									style={{ fontSize: "0.8rem", padding: "5px 10px" }}
									onClick={() => toggleTicker(ticker)}
								>
									{ticker}
								</button>
							))}
						</div>

						{/* Custom ticker input */}
						<div style={{ display: "flex", gap: 8, marginTop: 12 }}>
							<input
								type="text"
								value={manualTicker}
								onChange={(e) => setManualTicker(e.target.value.toUpperCase())}
								onKeyDown={(e) => e.key === "Enter" && addManualTicker()}
								placeholder="Add ticker (e.g. AMZN)"
								style={{
									flex: 1,
									padding: "8px 10px",
									borderRadius: 10,
									border: "1px solid var(--stroke)",
									fontSize: "0.9rem",
								}}
							/>
							<button type="button" className="button" style={{ padding: "8px 14px" }} onClick={addManualTicker}>
								Add
							</button>
						</div>

						{/* Selected summary */}
						{selectedTickers.length > 0 && (
							<div style={{ marginTop: 12 }}>
								<div className="result-label" style={{ marginBottom: 6 }}>
									Selected ({selectedTickers.length})
								</div>
								<div style={{ display: "flex", flexWrap: "wrap", gap: 6 }}>
									{selectedTickers.map((t) => (
										<span
											key={t}
											style={{
												display: "inline-flex",
												alignItems: "center",
												gap: 4,
												background: "rgba(31,138,112,0.12)",
												color: "#1f8a70",
												borderRadius: 999,
												padding: "4px 10px",
												fontSize: "0.8rem",
												fontWeight: 600,
											}}
										>
											{t}
											<button
												type="button"
												onClick={() => toggleTicker(t)}
												style={{
													background: "none",
													border: "none",
													cursor: "pointer",
													color: "#1f8a70",
													fontSize: "0.75rem",
													padding: 0,
													lineHeight: 1,
												}}
											>
												✕
											</button>
										</span>
									))}
								</div>
							</div>
						)}
					</div>

					{/* Parameters */}
					<div className="card" style={{ padding: "18px 20px" }}>
						<h3>Parameters</h3>
						<div className="grid-2" style={{ marginTop: 12 }}>
							<label className="field span-2">
								Optimisation start
								<input
									type="date"
									value={optStart}
									onChange={(e) => setOptStart(e.target.value)}
								/>
							</label>
							<label className="field span-2">
								Investment start (end of opt. window)
								<input
									type="date"
									value={optEnd}
									onChange={(e) => setOptEnd(e.target.value)}
								/>
							</label>
							<label className="field">
								Initial capital ($)
								<input
									type="number"
									min={100}
									step={100}
									value={initialCapital}
									onChange={(e) => setInitialCapital(Number(e.target.value))}
								/>
							</label>
							<label className="field">
								Rebalance every N days
								<input
									type="number"
									min={0}
									step={1}
									value={rebalanceFreq}
									onChange={(e) => setRebalanceFreq(Number(e.target.value))}
								/>
								<span style={{ fontSize: "0.75rem", color: "var(--ink-2)" }}>0 = static</span>
							</label>
							<label className="field">
								Max weight per asset
								<input
									type="number"
									min={0.05}
									max={1}
									step={0.05}
									value={maxShare}
									onChange={(e) => setMaxShare(Number(e.target.value))}
								/>
							</label>
							<label className="field">
								Min weight threshold
								<input
									type="number"
									min={0}
									max={0.5}
									step={0.01}
									value={minShare}
									onChange={(e) => setMinShare(Number(e.target.value))}
								/>
							</label>
						</div>

						<button
							type="button"
							className="button"
							style={{ marginTop: 16, width: "100%" }}
							onClick={handleRun}
							disabled={loading}
						>
							{loading ? "Running…" : "Run Backtest"}
						</button>

						{error && (
							<div
								style={{
									marginTop: 10,
									color: "#b42318",
									fontSize: "0.85rem",
									padding: "8px 12px",
									background: "rgba(180,35,24,0.06)",
									borderRadius: 10,
								}}
							>
								{error}
							</div>
						)}
					</div>
				</div>

				{/* ── Right panel: results ─────────────────────────────────── */}
				<div style={{ display: "flex", flexDirection: "column", gap: 20 }}>
					{loading && (
						<div
							className="card"
							style={{ textAlign: "center", padding: "40px 0", color: "var(--ink-2)" }}
						>
							<div style={{ fontSize: "2rem", marginBottom: 8 }}>⏳</div>
							Optimising and running backtest…
						</div>
					)}

					{result && (
						<>
							{/* Warnings */}
							{result.warnings.length > 0 && (
								<div
									style={{
										display: "flex",
										flexDirection: "column",
										gap: 6,
										padding: "12px 16px",
										background: "rgba(246,196,83,0.15)",
										border: "1px solid rgba(246,196,83,0.6)",
										borderRadius: 12,
									}}
								>
									{result.warnings.map((w, i) => (
										// biome-ignore lint/suspicious/noArrayIndexKey: static list
										<div key={i} style={{ fontSize: "0.85rem", color: "#7a5c00" }}>
											⚠ {w}
										</div>
									))}
								</div>
							)}

							{/* Summary bar */}
							<div
								style={{
									display: "flex",
									gap: 12,
									padding: "14px 18px",
									background: "var(--card-strong)",
									borderRadius: 16,
									border: "1px solid var(--stroke)",
									flexWrap: "wrap",
									alignItems: "center",
								}}
							>
								<div>
									<div className="result-label">Optimisation window</div>
									<div className="mono" style={{ fontSize: "0.9rem" }}>
										{result.opt_start} → {result.opt_end}
									</div>
								</div>
								<div style={{ marginLeft: "auto" }}>
									<div className="result-label">Final value</div>
									<div
										className="mono"
										style={{ fontSize: "1.1rem", color: returnPositive ? "#1f8a70" : "#b42318" }}
									>
										${lastValue.toLocaleString("en-US", { maximumFractionDigits: 2 })}
									</div>
								</div>
							</div>

							{/* Charts */}
							<div className="card" style={{ padding: "16px 18px" }}>
								<PortfolioChart
									portfolioValues={result.portfolio_values}
									sp500Values={result.sp500_values}
									initialCapital={initialCapital}
								/>
							</div>

							<div className="card" style={{ padding: "16px 18px" }}>
								<AllocationChart allocation={result.allocation} />
							</div>

							{/* Performance metrics */}
							<div className="card" style={{ padding: "18px 20px" }}>
								<h3>Performance Metrics</h3>
								<div className="result-grid" style={{ marginTop: 12 }}>
									<MetricCard
										label="Total Return"
										value={`${m!.total_return_pct >= 0 ? "+" : ""}${m!.total_return_pct.toFixed(2)}%`}
										positive={returnPositive}
									/>
									<MetricCard
										label="Annualised Return"
										value={`${m!.annualized_return_pct >= 0 ? "+" : ""}${m!.annualized_return_pct.toFixed(2)}%`}
										positive={(m!.annualized_return_pct ?? 0) >= 0}
									/>
									<MetricCard
										label="Max Drawdown"
										value={`${(m!.max_drawdown * 100).toFixed(2)}%`}
										positive={ddPositive}
									/>
									<MetricCard label="ATH" value={`$${m!.ath.toLocaleString("en-US", { maximumFractionDigits: 2 })}`} />
									<MetricCard label="ATL" value={`$${m!.atl.toLocaleString("en-US", { maximumFractionDigits: 2 })}`} />
									<MetricCard
										label="Sharpe (actual)"
										value={m!.sharpe_ratio.toFixed(3)}
										positive={sharpePositive}
									/>
									<MetricCard
										label="Sharpe (optimal)"
										value={m!.optimal_sharpe.toFixed(3)}
										positive={(m!.optimal_sharpe ?? 0) >= 1}
									/>
								</div>
							</div>

							{/* CAPM */}
							{(m!.alpha !== null || m!.beta !== null) && (
								<div className="card" style={{ padding: "18px 20px" }}>
									<h3>CAPM vs S&amp;P 500</h3>
									<div className="result-grid" style={{ marginTop: 12 }}>
										<MetricCard
											label="Alpha (daily)"
											value={m!.alpha !== null ? m!.alpha.toFixed(4) : "—"}
											positive={m!.alpha !== null ? m!.alpha >= 0 : undefined}
										/>
										<MetricCard
											label="Beta"
											value={m!.beta !== null ? m!.beta.toFixed(3) : "—"}
										/>
									</div>
									<p
										style={{
											marginTop: 12,
											fontSize: "0.85rem",
											color: "var(--ink-2)",
											lineHeight: 1.5,
										}}
									>
										{m!.beta !== null && (
											<>
												{m!.beta > 1
													? `Beta ${m!.beta.toFixed(2)}: the portfolio is more volatile than the market.`
													: m!.beta < 0
													? `Beta ${m!.beta.toFixed(2)}: the portfolio moves inversely to the market.`
													: `Beta ${m!.beta.toFixed(2)}: the portfolio is less volatile than the market.`}
											</>
										)}
										{m!.alpha !== null && (
											<>
												{" "}
												{m!.alpha > 0
													? "Positive alpha suggests the portfolio outperformed on a risk-adjusted basis."
													: "Negative alpha suggests the portfolio underperformed on a risk-adjusted basis."}
											</>
										)}
									</p>
								</div>
							)}

							{/* Allocation table */}
							<div className="card" style={{ padding: "18px 20px" }}>
								<h3>Asset Summary</h3>
								<div style={{ overflowX: "auto", marginTop: 12 }}>
									<table
										style={{
											width: "100%",
											borderCollapse: "collapse",
											fontSize: "0.88rem",
										}}
									>
										<thead>
											<tr
												style={{
													background: "rgba(16,20,24,0.04)",
													textAlign: "left",
												}}
											>
												{["Ticker", "Weight", "Start Price", "End Price", "Return"].map((h) => (
													<th
														key={h}
														style={{
															padding: "8px 12px",
															color: "var(--ink-2)",
															fontWeight: 600,
															borderBottom: "1px solid var(--stroke)",
														}}
													>
														{h}
													</th>
												))}
											</tr>
										</thead>
										<tbody>
											{result.allocation.map((row) => (
												<tr
													key={row.ticker}
													style={{ borderBottom: "1px solid var(--stroke)" }}
												>
													<td
														style={{
															padding: "8px 12px",
															fontWeight: 700,
															color: "var(--ink-0)",
															fontFamily: "monospace",
														}}
													>
														{row.ticker}
													</td>
													<td style={{ padding: "8px 12px" }}>
														{(row.weight * 100).toFixed(1)}%
													</td>
													<td style={{ padding: "8px 12px" }}>
														${row.start_price.toFixed(2)}
													</td>
													<td style={{ padding: "8px 12px" }}>
														${row.end_price.toFixed(2)}
													</td>
													<td
														style={{
															padding: "8px 12px",
															color:
																row.return_pct >= 0 ? "#1f8a70" : "#b42318",
															fontWeight: 600,
														}}
													>
														{row.return_pct >= 0 ? "+" : ""}
														{row.return_pct.toFixed(2)}%
													</td>
												</tr>
											))}
										</tbody>
									</table>
								</div>
							</div>
						</>
					)}

					{!loading && !result && (
						<div
							className="card"
							style={{
								textAlign: "center",
								padding: "56px 20px",
								color: "var(--ink-2)",
							}}
						>
							<div style={{ fontSize: "3rem", marginBottom: 12, opacity: 0.4 }}>📈</div>
							<p>Select tickers, configure parameters, and click <strong>Run Backtest</strong>.</p>
						</div>
					)}
				</div>
			</div>
		</div>
	);
}
