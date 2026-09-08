import { type ReactNode } from "react";
import { AxisBottom, AxisLeft } from "@visx/axis";
import { GridColumns, GridRows } from "@visx/grid";
import { Group } from "@visx/group";
import { scaleLinear } from "@visx/scale";
import { useChartTheme } from "./theme";
import { useMeasure } from "./useMeasure";

/**
 * Themed, responsive cartesian frame. Charts pass scale factories and a render
 * function; axes, grid and margins are handled once (WP 06 §3 — write the
 * shared frame BEFORE the first chart, not after the third).
 */
export function Cartesian({
	height = 280,
	margin = { top: 12, right: 16, bottom: 28, left: 44 },
	xDomain,
	yDomain,
	xFormat,
	yFormat,
	xLabel,
	yLabel,
	numXTicks = 6,
	numYTicks = 5,
	zeroLine = false,
	children,
}: {
	height?: number;
	margin?: { top: number; right: number; bottom: number; left: number };
	xDomain: [number, number];
	yDomain: [number, number];
	xFormat?: (v: number) => string;
	yFormat?: (v: number) => string;
	xLabel?: string;
	yLabel?: string;
	numXTicks?: number;
	numYTicks?: number;
	zeroLine?: boolean;
	children: (args: {
		xScale: (v: number) => number;
		yScale: (v: number) => number;
		innerWidth: number;
		innerHeight: number;
	}) => ReactNode;
}) {
	const t = useChartTheme();
	const [ref, { width }] = useMeasure<HTMLDivElement>();
	const w = width || 640;
	const innerWidth = Math.max(0, w - margin.left - margin.right);
	const innerHeight = Math.max(0, height - margin.top - margin.bottom);

	const xScale = scaleLinear({ domain: xDomain, range: [0, innerWidth] });
	const yScale = scaleLinear({
		domain: yDomain,
		range: [innerHeight, 0],
	});

	const tickLabel = {
		fill: t.inkMuted,
		fontSize: 10,
		fontFamily: "var(--font-mono)",
	};

	return (
		<div ref={ref} className="w-full">
			<svg width={w} height={height} role="img">
				<Group left={margin.left} top={margin.top}>
					<GridRows
						scale={yScale}
						width={innerWidth}
						numTicks={numYTicks}
						stroke={t.hairline}
						strokeWidth={1}
					/>
					<GridColumns
						scale={xScale}
						height={innerHeight}
						numTicks={numXTicks}
						stroke={t.hairline}
						strokeWidth={1}
					/>
					{zeroLine && yDomain[0] < 0 && yDomain[1] > 0 && (
						<line
							x1={0}
							x2={innerWidth}
							y1={yScale(0)}
							y2={yScale(0)}
							stroke={t.axis}
							strokeWidth={1}
						/>
					)}
					{children({
						xScale: (v: number) => xScale(v) ?? 0,
						yScale: (v: number) => yScale(v) ?? 0,
						innerWidth,
						innerHeight,
					})}
					<AxisBottom
						top={innerHeight}
						scale={xScale}
						numTicks={numXTicks}
						stroke={t.axis}
						tickStroke={t.axis}
						tickLabelProps={() => tickLabel}
						tickFormat={xFormat ? (v) => xFormat(Number(v)) : undefined}
						label={xLabel}
						labelProps={{ fill: t.inkMuted, fontSize: 10 }}
					/>
					<AxisLeft
						scale={yScale}
						numTicks={numYTicks}
						stroke={t.axis}
						tickStroke={t.axis}
						tickLabelProps={() => ({ ...tickLabel, textAnchor: "end", dx: -2 })}
						tickFormat={yFormat ? (v) => yFormat(Number(v)) : undefined}
						label={yLabel}
						labelProps={{ fill: t.inkMuted, fontSize: 10 }}
					/>
				</Group>
			</svg>
		</div>
	);
}
