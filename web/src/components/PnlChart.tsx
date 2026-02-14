import { useMemo, useState } from "react";

export type PnlPoint = { x: number; y: number };
export type PnlSeries = {
	id: string;
	label: string;
	color: string;
	points: PnlPoint[];
	opacity?: number;
};

type PnlChartProps = {
	title: string;
	series: PnlSeries[];
	spotNow?: number;
	showLegend?: boolean;
};

const padDomain = (min: number, max: number, padding = 0.12) => {
	if (min === max) {
		return [min - 1, max + 1];
	}
	const span = max - min;
	return [min - span * padding, max + span * padding];
};

const clamp = (value: number, min: number, max: number) =>
	Math.min(max, Math.max(min, value));

const interpolateAt = (points: PnlPoint[], x: number) => {
	if (points.length === 0) {
		return 0;
	}
	if (x <= points[0].x) {
		return points[0].y;
	}
	const last = points[points.length - 1];
	if (x >= last.x) {
		return last.y;
	}
	for (let i = 0; i < points.length - 1; i += 1) {
		const p0 = points[i];
		const p1 = points[i + 1];
		if (x >= p0.x && x <= p1.x) {
			const t = (x - p0.x) / (p1.x - p0.x);
			return p0.y + t * (p1.y - p0.y);
		}
	}
	return last.y;
};

export default function PnlChart({ title, series, spotNow, showLegend }: PnlChartProps) {
	const { xMin, xMax, yMin, yMax } = useMemo(() => {
		const all = series.flatMap((s) => s.points);
		const xs = all.map((p) => p.x);
		const ys = all.map((p) => p.y);
		const [x0, x1] = padDomain(Math.min(...xs), Math.max(...xs), 0.05);
		const [y0, y1] = padDomain(Math.min(...ys), Math.max(...ys), 0.2);
		return { xMin: x0, xMax: x1, yMin: y0, yMax: y1 };
	}, [series]);

	const [hover, setHover] = useState<{ xPx: number; yPx: number; spot: number } | null>(null);

	const width = 560;
	const height = 240;
	const padding = { top: 18, right: 20, bottom: 28, left: 42 };

	const scaleX = (x: number) =>
		padding.left + ((x - xMin) / (xMax - xMin)) * (width - padding.left - padding.right);
	const scaleY = (y: number) =>
		padding.top + (1 - (y - yMin) / (yMax - yMin)) * (height - padding.top - padding.bottom);

	const invertX = (xPx: number) =>
		xMin + ((xPx - padding.left) / (width - padding.left - padding.right)) * (xMax - xMin);
	const gridLines = Array.from({ length: 5 }, (_, i) => {
		const t = i / 4;
		const y = padding.top + t * (height - padding.top - padding.bottom);
		return y;
	});

	const xTicks = useMemo(() => {
		const span = xMax - xMin;
		const approxStep = span / 4;
		const magnitude = Math.pow(10, Math.floor(Math.log10(approxStep || 1)));
		const candidates = [1, 2, 5, 10].map((base) => base * magnitude);
		const step = candidates.reduce((best, value) =>
			Math.abs(value - approxStep) < Math.abs(best - approxStep) ? value : best
		);
		const first = Math.ceil(xMin / step) * step;
		const ticks: number[] = [];
		for (let t = first; t <= xMax + step * 0.5; t += step) {
			ticks.push(Number(t.toFixed(10)));
		}
		return ticks;
	}, [xMin, xMax]);

	const formatTick = (value: number) => {
		const span = Math.abs(xMax - xMin);
		if (span <= 10) return value.toFixed(2);
		if (span <= 50) return value.toFixed(1);
		return value.toFixed(0);
	};

	const hoverValues = useMemo(() => {
		if (!hover) return null;
		return series.map((s) => ({
			id: s.id,
			label: s.label,
			color: s.color,
			value: interpolateAt(s.points, hover.spot),
		}));
	}, [hover, series]);

	return (
		<div style={{ position: "relative" }}>
			<div style={{ display: "flex", justifyContent: "space-between", alignItems: "baseline" }}>
				<h3>{title}</h3>
				<span className="mono">PnL</span>
			</div>
			<svg
				viewBox={`0 0 ${width} ${height}`}
				width="100%"
				height="100%"
				onMouseMove={(event) => {
					const rect = event.currentTarget.getBoundingClientRect();
					const scaleXFactor = width / rect.width;
					const scaleYFactor = height / rect.height;
					const xPx = clamp((event.clientX - rect.left) * scaleXFactor, padding.left, width - padding.right);
					const yPx = clamp((event.clientY - rect.top) * scaleYFactor, padding.top, height - padding.bottom);
					const spot = invertX(xPx);
					setHover({ xPx, yPx, spot });
				}}
				onMouseLeave={() => setHover(null)}
			>
				<rect x={0} y={0} width={width} height={height} rx={16} fill="#fff" />
				{gridLines.map((y, idx) => (
					<line key={idx} x1={padding.left} y1={y} x2={width - padding.right} y2={y} stroke="rgba(16,20,24,0.08)" />
				))}
				<line
					x1={padding.left}
					y1={scaleY(0)}
					x2={width - padding.right}
					y2={scaleY(0)}
					stroke="rgba(16,20,24,0.18)"
					strokeDasharray="4 4"
				/>
				{spotNow !== undefined && spotNow >= xMin && spotNow <= xMax && (
					<line
						x1={scaleX(spotNow)}
						y1={padding.top}
						x2={scaleX(spotNow)}
						y2={height - padding.bottom}
						stroke="rgba(255,140,66,0.7)"
						strokeWidth={2}
					/>
				)}
				{series.map((s) => {
					const path = s.points
						.map((p, idx) => `${idx === 0 ? "M" : "L"} ${scaleX(p.x)} ${scaleY(p.y)}`)
						.join(" ");
					return (
						<path
							key={s.id}
							d={path}
							fill="none"
							stroke={s.color}
							strokeOpacity={s.opacity ?? 1}
							strokeWidth={2.5}
							strokeLinejoin="round"
							strokeLinecap="round"
						/>
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
						{hoverValues?.map((v) => (
							<circle
								key={v.id}
								cx={hover.xPx}
								cy={scaleY(v.value)}
								r={3.2}
								fill={v.color}
								stroke="#fff"
								strokeWidth={1.5}
							/>
						))}
					</>
				)}
				{xTicks.map((tick, idx) => (
					<g key={`x-tick-${idx}`}>
						<line
							x1={scaleX(tick)}
							y1={height - padding.bottom}
							x2={scaleX(tick)}
							y2={height - padding.bottom + 4}
							stroke="rgba(16,20,24,0.22)"
						/>
						<text
							x={scaleX(tick)}
							y={height - 6}
							fontSize={10}
							textAnchor="middle"
							fill="#59626a"
						>
							{formatTick(tick)}
						</text>
					</g>
				))}
				<text x={padding.left - 8} y={height - 2} fontSize={10} fill="#59626a">
					Spot
				</text>
			</svg>

			{hover && hoverValues && (
				<div className="pnl-hover-card">
					<div className="mono pnl-hover-title">Spot: {hover.spot.toFixed(2)}</div>
					{hoverValues.map((v) => (
						<div key={v.id} className="pnl-hover-row">
							<span className="pnl-hover-swatch" style={{ background: v.color }} />
							<span className="pnl-hover-label">{v.label}</span>
							<span className="mono">{v.value.toFixed(2)}</span>
						</div>
					))}
				</div>
			)}

			{showLegend && (
				<div className="legend">
					{series.map((s) => (
						<div key={s.id} className="legend-item">
							<span className="legend-swatch" style={{ background: s.color, opacity: s.opacity ?? 1 }} />
							<span>{s.label}</span>
						</div>
					))}
				</div>
			)}
		</div>
	);
}
