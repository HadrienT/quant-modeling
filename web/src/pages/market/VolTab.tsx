import Plot from "react-plotly.js";

export type VolSubTab = "raw" | "cleaned" | "local-vol";

type SurfaceData = {
	strikes: number[];
	maturities: number[];
	values: Array<Array<number | null>>;
	loading?: boolean;
	error?: string | null;
};

type VolTabProps = {
	equity: string;
	onEquityChange: (value: string) => void;
	tickers: string[];
	tickersLoading: boolean;
	surface: string;
	onSurfaceChange: (value: string) => void;
	subTab: VolSubTab;
	onSubTabChange: (tab: VolSubTab) => void;
	raw: SurfaceData;
	cleaned: SurfaceData;
	localVol: SurfaceData;
};

const SUB_TABS: { key: VolSubTab; label: string }[] = [
	{ key: "raw", label: "Raw IV" },
	{ key: "cleaned", label: "Cleaned IV" },
	{ key: "local-vol", label: "Local vol" },
];

const SURFACE_META: Record<VolSubTab, { title: string; zLabel: string; colorscale: string; note: string }> = {
	raw: {
		title: "Raw implied volatility surface",
		zLabel: "Implied vol",
		colorscale: "Viridis",
		note: "⚠️ This surface uses linear interpolation for visualization. It does NOT enforce arbitrage-free constraints. Not suitable for production pricing.",
	},
	cleaned: {
		title: "Cleaned & smoothed IV surface",
		zLabel: "Implied vol",
		colorscale: "Viridis",
		note: "Cleaned via liquidity, moneyness, calendar & butterfly arbitrage filters, then smoothed with a bicubic spline in log-moneyness / TTM space.",
	},
	"local-vol": {
		title: "Dupire local volatility surface",
		zLabel: "Local vol σ(S, t)",
		colorscale: "Hot",
		note: "Calibrated via the Gatheral (2004) formula applied to the cleaned IV surface. NaN cells filled with nearest-neighbour interpolation.",
	},
};

export const VolTab = ({
	equity,
	onEquityChange,
	tickers,
	tickersLoading,
	surface,
	onSurfaceChange,
	subTab,
	onSubTabChange,
	raw,
	cleaned,
	localVol,
}: VolTabProps) => {
	const data = subTab === "raw" ? raw : subTab === "cleaned" ? cleaned : localVol;
	const meta = SURFACE_META[subTab];
	const hasData = data.strikes.length > 0 && !data.loading && !data.error;

	// Replace null with NaN so Plotly's WebGL renderer doesn't choke
	const safeZ = data.values.map((row) => row.map((v) => (v === null ? NaN : v)));

	return (
		<section className="card">
			{/* Equity + surface selector */}
			<div className="grid-2" style={{ marginBottom: 18 }}>
				<label className="field">
					Equity
					<input
						type="text"
						list="ticker-options-vol"
						placeholder="Type or select ticker..."
						value={equity}
						onChange={(e) => onEquityChange(e.target.value.toUpperCase())}
						disabled={tickersLoading}
						style={{ textTransform: "uppercase" }}
					/>
					<datalist id="ticker-options-vol">
						{tickers.map((ticker) => (
							<option key={ticker} value={ticker} />
						))}
					</datalist>
				</label>
			</div>
			{subTab === "raw" && (
				<div className="grid-2" style={{ marginBottom: 18 }}>
					<label className="field">
						Vol surface
						<select value={surface} onChange={(e) => onSurfaceChange(e.target.value)}>
							<option value="Mid vol">Mid vol</option>
							<option value="Bid vol">Bid vol</option>
							<option value="Ask vol">Ask vol</option>
						</select>
					</label>
				</div>
			)}

			{/* Sub-tab buttons */}
			<div className="vol-sub-tabs" style={{ marginBottom: 18 }}>
				{SUB_TABS.map(({ key, label }) => (
					<button
						key={key}
						className={`button ${subTab === key ? "primary" : "secondary"}`}
						onClick={() => onSubTabChange(key)}
					>
						{label}
					</button>
				))}
			</div>

			<h2>{meta.title}</h2>

			{data.error && <p className="price-muted">{data.error}</p>}
			{data.loading && <p className="price-muted">Loading surface…</p>}

			{hasData && (
				<div className="price-muted" style={{ marginBottom: 12, padding: "10px 12px", background: subTab === "raw" ? "#fff3cd" : "#d1ecf1", borderRadius: "8px", border: `1px solid ${subTab === "raw" ? "#ffc107" : "#bee5eb"}` }}>
					{subTab === "raw" ? "⚠️" : "ℹ️"} {meta.note}
				</div>
			)}

			<div className="surface-plot">
				<Plot
					data={[
						{
							type: "surface",
							x: data.strikes,
							y: data.maturities,
							z: safeZ,
							colorscale: meta.colorscale,
							showscale: true,
							colorbar: { title: { text: meta.zLabel } },
						},
					]}
					layout={{
						title: "",
						autosize: true,
						margin: { l: 0, r: 0, t: 0, b: 0 },
						scene: {
							xaxis: { title: "Strike", autorange: "reversed" },
							yaxis: { title: "Maturity (y)" },
							zaxis: { title: meta.zLabel },
							camera: { eye: { x: 1.2, y: 1.2, z: 0.8 } },
						},
					}}
					config={{ responsive: true, displayModeBar: false }}
					style={{ width: "100%", height: "100%" }}
				/>
			</div>

			<p className="price-muted" style={{ marginTop: 12 }}>
				Drag to rotate the surface. Use this view to choose the curve/surface later in pricing.
			</p>
		</section>
	);
};
