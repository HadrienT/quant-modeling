import { useMemo, useState } from "react";
import ChartHoverCard from "../../components/ChartHoverCard";
import { interpolateAt } from "../../utils/chartUtils";
import { CurvePoint } from "./types";

type CurveChartProps = {
	title: string;
	series: { label: string; color: string; points: CurvePoint[] }[];
};

type RatesTabProps = {
	curve: string;
	onCurveChange: (value: string) => void;
	zeroCurve: CurvePoint[];
	curveView: CurveView;
	onCurveViewChange: (value: CurveView) => void;
	fixedPeriodYears: number;
	onFixedPeriodYearsChange: (value: number) => void;
	loading: boolean;
	error: string | null;
};

type MaturityWindow = "5Y" | "10Y" | "15Y";
type CurveView = "zero" | "forward";

const maxYearsByWindow: Record<MaturityWindow, number> = {
	"5Y": 5,
	"10Y": 10,
	"15Y": 15
};

const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));

const interpolateCurve = (points: CurvePoint[], x: number) => interpolateAt(points, x);

const formatTenor = (value: number) => {
	if (value >= 1) {
		return Number.isInteger(value) ? `${value.toFixed(0)}Y` : `${value.toFixed(1)}Y`;
	}
	if (value >= 1 / 12) {
		return `${Math.round(value * 12)}M`;
	}
	return `${Math.max(1, Math.round(value * 360))}D`;
};

const CurveChart = ({ title, series }: CurveChartProps) => {
	const width = 520;
	const height = 180;
	const padding = { top: 18, right: 20, bottom: 28, left: 46 };
	const allPoints = series.flatMap((s) => s.points);
	const hasData = allPoints.length > 0;
	const xs = hasData ? allPoints.map((p) => p.x) : [0, 1];
	const ys = hasData ? allPoints.map((p) => p.y) : [0, 1];
	const xMin = Math.min(...xs);
	const xMax = Math.max(...xs);
	const yMinRaw = Math.min(...ys);
	const yMaxRaw = Math.max(...ys);
	const ySpan = Math.max(yMaxRaw - yMinRaw, 0.1);
	const yPad = ySpan * 0.12;
	const yMin = yMinRaw - yPad;
	const yMax = yMaxRaw + yPad;
	const scaleX = (x: number) =>
		padding.left + ((x - xMin) / (xMax - xMin)) * (width - padding.left - padding.right);
	const scaleY = (y: number) =>
		padding.top + (1 - (y - yMin) / (yMax - yMin)) * (height - padding.top - padding.bottom);
	const tickCount = 4;
	const ticks = Array.from({ length: tickCount + 1 }, (_, idx) => yMin + ((yMax - yMin) / tickCount) * idx);
	const tenorTicks = Array.from(new Set(allPoints.map((p) => p.x))).sort((a, b) => a - b);
	const displayedTenorTicks = tenorTicks.reduce<{ ticks: number[]; lastKeptX: number }>((acc, tenor, idx) => {
		const minLabelSpacingPx = 34;
		const x = scaleX(tenor);
		const isEdge = idx === 0 || idx === tenorTicks.length - 1;
		if (isEdge || x - acc.lastKeptX >= minLabelSpacingPx) {
			return { ticks: [...acc.ticks, tenor], lastKeptX: x };
		}
		return acc;
	}, { ticks: [], lastKeptX: -Infinity }).ticks;

	const [hover, setHover] = useState<{ xPx: number; yPx: number; xVal: number } | null>(null);

	const hoverValues = useMemo(() => {
		if (!hover) return null;
		return series.map((s) => ({
			label: s.label,
			color: s.color,
			value: interpolateCurve(s.points, hover.xVal)
		}));
	}, [hover, series]);

	if (!hasData) {
		return (
			<div className="curve-panel" style={{ position: "relative" }}>
				<div className="curve-panel-header">
					<h3>{title}</h3>
					<span className="mono">% rate</span>
				</div>
				<p className="price-muted">No curve data available.</p>
			</div>
		);
	}

	return (
		<div className="curve-panel" style={{ position: "relative" }}>
			<div className="curve-panel-header">
				<h3>{title}</h3>
				<span className="mono">% rate</span>
			</div>
			<svg viewBox={`0 0 ${width} ${height}`} width="100%" height="100%">
				<rect x={0} y={0} width={width} height={height} rx={16} fill="#fbfbfb" />
				{ticks.map((tick) => {
					const y = scaleY(tick);
					return (
						<g key={`tick-${tick}`}>
							<line
								x1={padding.left}
								y1={y}
								x2={width - padding.right}
								y2={y}
								stroke="rgba(16,20,24,0.08)"
								strokeDasharray="4 6"
							/>
							<text
								x={padding.left - 8}
								y={y + 4}
								textAnchor="end"
								fontSize={10}
								fill="#5c6f7b"
							>
								{tick.toFixed(2)}
							</text>
						</g>
					);
				})}
				<line
					x1={padding.left}
					y1={height - padding.bottom}
					x2={width - padding.right}
					y2={height - padding.bottom}
					stroke="rgba(16,20,24,0.16)"
				/>
				{displayedTenorTicks.map((tenor) => {
					const x = scaleX(tenor);
					return (
						<g key={`tenor-${tenor}`}>
							<line
								x1={x}
								y1={height - padding.bottom}
								x2={x}
								y2={height - padding.bottom + 5}
								stroke="rgba(16,20,24,0.25)"
							/>
							<text
								x={x}
								y={height - padding.bottom + 18}
								textAnchor="middle"
								fontSize={10}
								fill="#5c6f7b"
							>
								{formatTenor(tenor)}
							</text>
						</g>
					);
				})}
				{hover && (
					<>
						<line
							x1={hover.xPx}
							y1={padding.top}
							x2={hover.xPx}
							y2={height - padding.bottom}
							stroke="rgba(16,20,24,0.22)"
							strokeDasharray="4 4"
						/>
						<line
							x1={padding.left}
							y1={hover.yPx}
							x2={width - padding.right}
							y2={hover.yPx}
							stroke="rgba(16,20,24,0.22)"
							strokeDasharray="4 4"
						/>
					</>
				)}
				{series.map((s) => {
					const path = s.points
						.map((p, idx) => `${idx === 0 ? "M" : "L"} ${scaleX(p.x)} ${scaleY(p.y)}`)
						.join(" ");
					return (
						<g key={s.label}>
							<path
								d={path}
								fill="none"
								stroke={s.color}
								strokeWidth={2.8}
								strokeLinejoin="round"
								strokeLinecap="round"
							/>
							{s.points.map((p, idx) => (
								<circle
									key={`${s.label}-pt-${idx}`}
									cx={scaleX(p.x)}
									cy={scaleY(p.y)}
									r={3.4}
									fill={s.color}
									stroke="#fff"
									strokeWidth={1.2}
								/>
							))}
						</g>
					);
				})}
				{hoverValues?.map((v, idx) => (
					<circle
						key={`${v.label}-${idx}`}
						cx={hover?.xPx ?? 0}
						cy={scaleY(v.value)}
						r={3.2}
						fill={v.color}
						stroke="#fff"
						strokeWidth={1.5}
					/>
				))}
				<rect
					x={padding.left}
					y={padding.top}
					width={width - padding.left - padding.right}
					height={height - padding.top - padding.bottom}
					fill="transparent"
					onMouseMove={(event) => {
						const rect = event.currentTarget.getBoundingClientRect();
						const plotWidth = width - padding.left - padding.right;
						const plotHeight = height - padding.top - padding.bottom;
						const scaleXFactor = plotWidth / rect.width;
						const scaleYFactor = plotHeight / rect.height;
						const xPx = clamp(
							padding.left + (event.clientX - rect.left) * scaleXFactor,
							padding.left,
							width - padding.right
						);
						const yPx = clamp(
							padding.top + (event.clientY - rect.top) * scaleYFactor,
							padding.top,
							height - padding.bottom
						);
						const xVal = xMin + ((xPx - padding.left) / (width - padding.left - padding.right)) * (xMax - xMin);
						setHover({ xPx, yPx, xVal });
					}}
					onMouseLeave={() => setHover(null)}
				/>
			</svg>
			{hover && hoverValues && (
				<ChartHoverCard
					title={`Tenor: ${hover.xVal.toFixed(2)}y`}
					items={hoverValues.map((v) => ({
						label: v.label,
						color: v.color,
						value: `${v.value.toFixed(2)}%`
					}))}
				/>
			)}
			<div className="legend">
				{series.map((s) => (
					<div key={s.label} className="legend-item">
						<span className="legend-swatch" style={{ background: s.color }} />
						{s.label}
					</div>
				))}
			</div>
		</div>
	);
};

export const RatesTab = ({
	curve,
	onCurveChange,
	zeroCurve,
	curveView,
	onCurveViewChange,
	fixedPeriodYears,
	onFixedPeriodYearsChange,
	loading,
	error
}: RatesTabProps) => {
	const [maturityWindow, setMaturityWindow] = useState<MaturityWindow>("5Y");

	const baseCurve = zeroCurve;

	const filteredCurve = useMemo(() => {
		const maxYears = maxYearsByWindow[maturityWindow];
		if (baseCurve.length === 0) {
			return baseCurve;
		}

		const sorted = [...baseCurve].sort((a, b) => a.x - b.x);
		const filtered = sorted.filter((point) => point.x <= maxYears);

		const hasExactBoundary = sorted.some((point) => point.x === maxYears);
		if (hasExactBoundary) {
			return filtered;
		}

		let leftPoint: CurvePoint | null = null;
		let rightPoint: CurvePoint | null = null;
		for (let idx = 1; idx < sorted.length; idx += 1) {
			const left = sorted[idx - 1];
			const right = sorted[idx];
			if (left.x < maxYears && right.x > maxYears) {
				leftPoint = left;
				rightPoint = right;
				break;
			}
		}

		if (leftPoint && rightPoint) {
			const weight = (maxYears - leftPoint.x) / (rightPoint.x - leftPoint.x);
			const interpolatedY = leftPoint.y + weight * (rightPoint.y - leftPoint.y);
			return [...filtered, { x: maxYears, y: interpolatedY }];
		}

		return filtered.length > 0 ? filtered : sorted;
	}, [baseCurve, maturityWindow]);

	return (
		<section className="card">
			<div className="grid-2" style={{ marginBottom: 18 }}>
				<label className="field">
					Rate curve
					<select value={curve} onChange={(event) => onCurveChange(event.target.value)}>
						<option value="SOFR">SOFR</option>
						<option value="OIS">OIS</option>
					</select>
				</label>
			</div>
			<div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: 16 }}>
				<h2>Rate curves</h2>
				<div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
					<button
						type="button"
						className={`button ${curveView === "zero" ? "primary" : "secondary"}`}
						onClick={() => onCurveViewChange("zero")}
					>
						Zero
					</button>
					<button
						type="button"
						className={`button ${curveView === "forward" ? "primary" : "secondary"}`}
						onClick={() => onCurveViewChange("forward")}
					>
						Forward
					</button>
					{(["5Y", "10Y", "15Y"] as MaturityWindow[]).map((label) => (
						<button
							key={label}
							type="button"
							className={`button ${maturityWindow === label ? "primary" : "secondary"}`}
							onClick={() => setMaturityWindow(label)}
						>
							{label}
						</button>
					))}
				</div>
			</div>
			{curveView === "forward" && (
				<div className="grid-2" style={{ marginTop: 12, marginBottom: 8 }}>
					<label className="field">
						Forward fixed period (years)
						<input
							type="number"
							min="0.01"
							step="0.25"
							value={fixedPeriodYears}
							onChange={(event) => onFixedPeriodYearsChange(Math.max(0.01, Number(event.target.value) || 0.5))}
						/>
					</label>
				</div>
			)}
			{loading && <p className="price-muted">Loading rates curve...</p>}
			{error && <p className="price-muted">{error}</p>}
			<CurveChart
				title={`${curveView === "zero" ? "Zero" : "Forward"} curve (up to ${maturityWindow})`}
				series={[
					{ label: curveView === "zero" ? "Zero" : `Forward (${fixedPeriodYears}Y)`, color: "#1f8a70", points: filteredCurve }
				]}
			/>
			<p className="price-muted" style={{ marginTop: 12 }}>
				Rates shown in percent. Use the curve selector in pricing to apply consistent discounting.
			</p>
		</section>
	);
};
