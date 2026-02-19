import Plot from "react-plotly.js";
import { PricePoint, TimeRange } from "./types";

type PriceTabProps = {
	equity: string;
	onEquityChange: (value: string) => void;
	tickers: string[];
	tickersLoading: boolean;
	debouncedEquity: string;
	range: TimeRange;
	onRangeChange: (value: TimeRange) => void;
	yAxisPadding: number;
	onPaddingChange: (value: number) => void;
	priceError: string | null;
	priceLoading: boolean;
	latest?: PricePoint;
	previous?: PricePoint;
	change: number;
	changePct: number;
	visibleSeries: PricePoint[];
	yAxisRange: number[] | undefined;
};

const formatPrice = (value: number) => value.toFixed(2);

export const PriceTab = ({
	equity,
	onEquityChange,
	tickers,
	tickersLoading,
	debouncedEquity,
	range,
	onRangeChange,
	yAxisPadding,
	onPaddingChange,
	priceError,
	priceLoading,
	latest,
	previous,
	change,
	changePct,
	visibleSeries,
	yAxisRange
}: PriceTabProps) => (
	<section className="card" style={{ marginBottom: 24 }}>
		<div className="grid-2" style={{ marginBottom: 18 }}>
			<label className="field">
				Equity
				<input
					type="text"
					list="ticker-options"
					placeholder="Type or select ticker..."
					value={equity}
					onChange={(e) => onEquityChange(e.target.value.toUpperCase())}
					disabled={tickersLoading}
					style={{ textTransform: "uppercase" }}
				/>
				<datalist id="ticker-options">
					{tickers.map((ticker) => (
						<option key={ticker} value={ticker} />
					))}
				</datalist>
			</label>
		</div>
		<div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: 16 }}>
			<div>
				<h2 style={{ marginBottom: 6 }}>Price tape</h2>
				<p className="price-muted">
					Live window for {debouncedEquity}. Hover to inspect, drag to zoom.
				</p>
			</div>
			<div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
				{(["1M", "3M", "6M", "YTD", "1Y", "2Y"] as TimeRange[]).map((label) => (
					<button
						key={label}
						className={`button ${range === label ? "primary" : "secondary"}`}
						onClick={() => onRangeChange(label)}
					>
						{label}
					</button>
				))}
			</div>
		</div>
		<div style={{ marginTop: 16, display: "flex", alignItems: "center", gap: 12 }}>
			<label style={{ display: "flex", alignItems: "center", gap: 8, fontSize: 14, color: "#5c6f7b" }}>
				Y-axis padding:
				<input
					type="range"
					min="0"
					max="100"
					step="1"
					value={yAxisPadding}
					onChange={(e) => onPaddingChange(Number(e.target.value))}
					style={{ width: 150 }}
				/>
				<span className="mono" style={{ minWidth: 45 }}>{yAxisPadding}%</span>
			</label>
		</div>
		{priceError && <p className="price-muted">{priceError}</p>}
		<div
			style={{
				display: "grid",
				gridTemplateColumns: "repeat(auto-fit, minmax(180px, 1fr))",
				gap: 12,
				marginTop: 18
			}}
		>
			<div className="hero-card" style={{ minHeight: 86 }}>
				<p className="price-muted">Last</p>
				<h3>{latest ? formatPrice(latest.price) : "-"}</h3>
			</div>
			<div className="hero-card" style={{ minHeight: 86 }}>
				<p className="price-muted">Change (1D)</p>
				<h3>
					{latest && previous ? `${change >= 0 ? "+" : ""}${formatPrice(change)}` : "-"}
				</h3>
				<p className="price-muted">
					{latest && previous ? `${changePct >= 0 ? "+" : ""}${changePct.toFixed(2)}%` : ""}
				</p>
			</div>
			<div className="hero-card" style={{ minHeight: 86 }}>
				<p className="price-muted">Window</p>
				<h3>{range}</h3>
				<p className="price-muted">{visibleSeries.length} sessions</p>
			</div>
		</div>
		<div style={{ height: 360, marginTop: 18 }}>
			<Plot
				data={[
					{
						x: visibleSeries.map((p) => p.date),
						y: visibleSeries.map((p) => p.price),
						type: "scatter",
						mode: "lines",
						line: { color: "#1f8a70", width: 2.4 },
						fill: "tozeroy",
						fillcolor: "rgba(31,138,112,0.12)",
						name: debouncedEquity
					}
				]}
				layout={{
					autosize: true,
					margin: { l: 50, r: 30, t: 10, b: 40 },
					hovermode: "x unified",
					paper_bgcolor: "rgba(0,0,0,0)",
					plot_bgcolor: "rgba(0,0,0,0)",
					xaxis: {
						showgrid: false,
						tickfont: { color: "#5c6f7b" },
						rangeslider: { visible: true, thickness: 0.08 }
					},
					yaxis: {
						tickfont: { color: "#5c6f7b" },
						gridcolor: "rgba(18,32,45,0.08)",
						zeroline: false,
						range: yAxisRange
					}
				}}
				config={{ responsive: true, displayModeBar: false }}
				style={{ width: "100%", height: "100%" }}
			/>
		</div>
		{priceLoading && <p className="price-muted">Loading prices...</p>}
	</section>
);
