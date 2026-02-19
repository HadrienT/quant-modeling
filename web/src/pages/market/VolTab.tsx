import Plot from "react-plotly.js";

type VolTabProps = {
	equity: string;
	onEquityChange: (value: string) => void;
	tickers: string[];
	tickersLoading: boolean;
	surface: string;
	onSurfaceChange: (value: string) => void;
	strikes: number[];
	maturities: number[];
	surfaceValues: Array<Array<number | null>>;
	loading?: boolean;
	error?: string | null;
};

export const VolTab = ({
	equity,
	onEquityChange,
	tickers,
	tickersLoading,
	surface,
	onSurfaceChange,
	strikes,
	maturities,
	surfaceValues,
	loading,
	error
}: VolTabProps) => (
	<section className="card">
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
		<div className="grid-2" style={{ marginBottom: 18 }}>
			<label className="field">
				Vol surface
				<select value={surface} onChange={(event) => onSurfaceChange(event.target.value)}>
					<option value="Mid vol">Mid vol</option>
					<option value="Bid vol">Bid vol</option>
					<option value="Ask vol">Ask vol</option>
				</select>
			</label>
		</div>
		<h2>Implied volatility surface</h2>
		{error && <p className="price-muted">{error}</p>}
		{loading && <p className="price-muted">Loading IV surface...</p>}
		{!loading && !error && strikes.length > 0 && (
			<div className="price-muted" style={{ marginBottom: 12, padding: "10px 12px", background: "#fff3cd", borderRadius: "8px", border: "1px solid #ffc107" }}>
				⚠️ <strong>Note:</strong> This surface uses linear interpolation for visualization. It does NOT enforce arbitrage-free constraints (convexity, butterfly spreads, calendar arbitrage). Not suitable for production pricing.
			</div>
		)}
		<div className="surface-plot">
			<Plot
				data={[
					{
						type: "surface",
						x: strikes,
						y: maturities,
						z: surfaceValues,
						colorscale: "Viridis",
						showscale: true,
						colorbar: { title: { text: "Implied vol" } }
					}
				]}
				layout={{
					title: "",
					autosize: true,
					margin: { l: 0, r: 0, t: 0, b: 0 },
					scene: {
						xaxis: { title: "Strike", autorange: "reversed" },
						yaxis: { title: "Maturity (y)" },
						zaxis: { title: "Vol" },
						camera: { eye: { x: 1.2, y: 1.2, z: 0.8 } }
					}
				}}
				config={{
					responsive: true,
					displayModeBar: false
				}}
				style={{ width: "100%", height: "100%" }}
			/>
		</div>
		<p className="price-muted" style={{ marginTop: 12 }}>
			Drag to rotate the surface. Use this view to choose the curve/surface later in pricing.
		</p>
	</section>
);
