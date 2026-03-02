import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import ChartHoverCard from "./ChartHoverCard";
import { interpolateAt } from "../utils/chartUtils";

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
	showFill?: boolean;
	breakevens?: number[];
	chartHeight?: number;
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

const interpolatePnl = (points: PnlPoint[], x: number) => interpolateAt(points, x);

export default function PnlChart({ title, series, spotNow, showLegend, showFill, breakevens, chartHeight }: PnlChartProps) {
	const { xMinData, xMaxData, yMinData, yMaxData } = useMemo(() => {
		const all = series.flatMap((s) => s.points);
		const xs = all.map((p) => p.x);
		const ys = all.map((p) => p.y);
		const [x0, x1] = padDomain(Math.min(...xs), Math.max(...xs), 0.05);
		const [y0, y1] = padDomain(Math.min(...ys), Math.max(...ys), 0.2);
		return { xMinData: x0, xMaxData: x1, yMinData: y0, yMaxData: y1 };
	}, [series]);

	const [yZoom, setYZoom] = useState(1);
	const [xOffset, setXOffset] = useState(0);
	const dragRef = useRef<{ startClientX: number; offsetSnap: number } | null>(null);
	const svgRef = useRef<SVGSVGElement | null>(null);

	// Apply y-zoom anchored at y=0 so the zero line stays fixed
	const yMin = yMinData * yZoom;
	const yMax = yMaxData * yZoom;
	const xMin = xMinData + xOffset;
	const xMax = xMaxData + xOffset;

	// Reset view state when data changes
	const prevDataRef = useRef({ xMinData, xMaxData });
	if (prevDataRef.current.xMinData !== xMinData || prevDataRef.current.xMaxData !== xMaxData) {
		prevDataRef.current = { xMinData, xMaxData };
		if (xOffset !== 0) setXOffset(0);
		if (yZoom !== 1) setYZoom(1);
	}

	const [hover, setHover] = useState<{ xPx: number; yPx: number; spot: number } | null>(null);

	const width = 560;
	const height = chartHeight ?? 300;
	const padding = { top: 18, right: 20, bottom: 28, left: 48 };

	const scaleX = (x: number) =>
		padding.left + ((x - xMin) / (xMax - xMin)) * (width - padding.left - padding.right);
	const scaleY = (y: number) =>
		padding.top + (1 - (y - yMin) / (yMax - yMin)) * (height - padding.top - padding.bottom);

	const invertX = (xPx: number) =>
		xMin + ((xPx - padding.left) / (width - padding.left - padding.right)) * (xMax - xMin);

	// Native non-passive wheel listener so preventDefault actually stops page scroll
	useEffect(() => {
		const el = svgRef.current;
		if (!el) return;
		const onWheel = (e: WheelEvent) => {
			e.preventDefault();
			e.stopPropagation();
			const factor = e.deltaY > 0 ? 1.08 : 0.92;
			setYZoom((prev) => clamp(prev * factor, 0.1, 10));
		};
		el.addEventListener("wheel", onWheel, { passive: false });
		return () => el.removeEventListener("wheel", onWheel);
	}, []);

	const handleMouseDown = useCallback(
		(e: React.MouseEvent<SVGSVGElement>) => {
			e.preventDefault();
			dragRef.current = {
				startClientX: e.clientX,
				offsetSnap: xOffset,
			};
		},
		[xOffset],
	);

	const handleMouseMoveOrDrag = useCallback(
		(e: React.MouseEvent<SVGSVGElement>) => {
			const rect = e.currentTarget.getBoundingClientRect();
			const scaleXFactor = width / rect.width;
			const scaleYFactor = height / rect.height;

			if (dragRef.current) {
				const dxPx = e.clientX - dragRef.current.startClientX;
				const plotW = rect.width * ((width - padding.left - padding.right) / width);
				const span = xMaxData - xMinData;
				const dxData = -(dxPx / plotW) * span;
				setXOffset(dragRef.current.offsetSnap + dxData);
				return;
			}

			const xPx = clamp((e.clientX - rect.left) * scaleXFactor, padding.left, width - padding.right);
			const yPx = clamp((e.clientY - rect.top) * scaleYFactor, padding.top, height - padding.bottom);
			const spot = invertX(xPx);
			setHover({ xPx, yPx, spot });
		},
		[width, height, padding, xMinData, xMaxData, invertX],
	);

	const handleMouseUp = useCallback(() => {
		dragRef.current = null;
	}, []);
	const gridLines = Array.from({ length: 5 }, (_, i) => {
		const t = i / 4;
		const y = padding.top + t * (height - padding.top - padding.bottom);
		const val = yMax - t * (yMax - yMin);
		return { y, val };
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
			value: interpolatePnl(s.points, hover.spot),
		}));
	}, [hover, series]);

	return (
		<div style={{ position: "relative" }}>
			<div style={{ display: "flex", justifyContent: "space-between", alignItems: "baseline" }}>
				<h3>{title}</h3>
				<span className="mono">PnL</span>
			</div>
			<svg
				ref={svgRef}
				viewBox={`0 0 ${width} ${height}`}
				width="100%"
				style={{ display: 'block', cursor: dragRef.current ? 'grabbing' : 'grab' }}
				onMouseDown={handleMouseDown}
				onMouseMove={handleMouseMoveOrDrag}
				onMouseUp={handleMouseUp}
				onMouseLeave={() => { dragRef.current = null; setHover(null); }}
			>
				<rect x={0} y={0} width={width} height={height} rx={16} fill="#fff" />
				{gridLines.map((gl, idx) => (
					<g key={`grid-${idx}`}>
						<line x1={padding.left} y1={gl.y} x2={width - padding.right} y2={gl.y} stroke="rgba(16,20,24,0.08)" />
						<text x={padding.left - 6} y={gl.y + 3.5} fontSize={9} textAnchor="end" fill="#59626a">
							{Math.abs(gl.val) < 0.005 ? "0" : gl.val.toFixed(1)}
						</text>
					</g>
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

				{/* fill profit / loss areas for first series */}
				{showFill && series.length > 0 && (() => {
					const pts = series[0].points;
					if (pts.length < 2) return null;
					const zeroY0 = clamp(scaleY(0), padding.top, height - padding.bottom);
					const fillD =
						pts.map((p, i) => `${i === 0 ? "M" : "L"}${scaleX(p.x)},${scaleY(p.y)}`).join(" ") +
						` L${scaleX(pts[pts.length - 1].x)},${zeroY0} L${scaleX(pts[0].x)},${zeroY0} Z`;
					return (
						<>
							<defs>
								<clipPath id="clip-profit">
									<rect x={padding.left} y={padding.top} width={width - padding.left - padding.right} height={Math.max(zeroY0 - padding.top, 0)} />
								</clipPath>
								<clipPath id="clip-loss">
									<rect x={padding.left} y={zeroY0} width={width - padding.left - padding.right} height={Math.max(height - padding.bottom - zeroY0, 0)} />
								</clipPath>
							</defs>
							<path d={fillD} fill="rgba(31,138,112,0.10)" clipPath="url(#clip-profit)" />
							<path d={fillD} fill="rgba(180,35,24,0.10)" clipPath="url(#clip-loss)" />
						</>
					);
				})()}

				{/* breakeven markers */}
				{breakevens && breakevens.map((be, i) => {
					const bx = scaleX(be);
					const by = clamp(scaleY(0), padding.top, height - padding.bottom);
					return (
						<g key={`be-${i}`}>
							<circle cx={bx} cy={by} r={4} fill="#ff8c42" stroke="#fff" strokeWidth={1.5} />
							<text x={bx} y={by - 8} fontSize={9} textAnchor="middle" fill="#ff8c42" fontWeight={600}>
								{be.toFixed(1)}
							</text>
						</g>
					);
				})}

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
				<ChartHoverCard
					title={`Spot: ${hover.spot.toFixed(2)}`}
					titleClassName="mono pnl-hover-title"
					valueClassName="mono"
					items={hoverValues.map((v) => ({
						key: v.id,
						label: v.label,
						color: v.color,
						value: v.value.toFixed(2)
					}))}
				/>
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
