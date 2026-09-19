import { useMemo } from "react";
import { AxisBottom, AxisLeft } from "@visx/axis";
import { Group } from "@visx/group";
import { scaleLinear } from "@visx/scale";
import { Line, LinePath } from "@visx/shape";
import { ChartFrame } from "../ChartFrame";
import { useChartTheme } from "../theme";
import { useMeasure } from "../useMeasure";

export type SimulationPathPoint = { t: number; value: number };

/**
 * Spaghetti plot of simulated underlying paths — one low-opacity line per
 * path, no per-path legend (unlike ConvergenceChart: with a few dozen
 * paths a legend entry each would be noise, not information), plus the
 * cross-path mean at each step drawn on top in red -- the one line that
 * IS worth calling out individually. `revealCount` truncates every path
 * (and the mean) to its first N points, which is what the caller uses to
 * animate "the simulation drawing itself" client-side (see
 * features/simulation/SimulationPage.tsx) rather than building any
 * server-side streaming.
 */
export function SimulationPathsChart({
	paths,
	revealCount,
	title = "Simulated paths",
	subtitle,
	isLoading,
	error,
}: {
	paths?: SimulationPathPoint[][];
	revealCount?: number;
	title?: string;
	subtitle?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const [ref, { width }] = useMeasure<HTMLDivElement>();
	const w = width || 640;
	const height = 340;
	const m = { top: 10, right: 16, bottom: 30, left: 56 };
	const iw = Math.max(0, w - m.left - m.right);
	const ih = height - m.top - m.bottom;

	const all = paths?.flat() ?? [];
	const isEmpty = all.length === 0;

	const tMax = Math.max(...all.map((p) => p.t), 1e-9);
	const vMin = Math.min(...all.map((p) => p.value));
	const vMax = Math.max(...all.map((p) => p.value));
	const pad = (vMax - vMin) * 0.05 || vMax * 0.02 || 1;

	const xs = scaleLinear({ domain: [0, tMax], range: [0, iw] });
	const ys = scaleLinear({
		domain: [vMin - pad, vMax + pad],
		range: [ih, 0],
	});

	const color = t.series[0]!;
	const revealed = paths?.map((p) =>
		revealCount !== undefined ? p.slice(0, revealCount + 1) : p,
	);

	// Mean across paths at each step -- every path shares the same time
	// grid (built from the same server response), so a plain index-wise
	// average is exact, not an interpolation.
	const meanPath = useMemo(() => {
		if (!paths || paths.length === 0) return [];
		const nSteps = paths[0]!.length;
		return Array.from({ length: nSteps }, (_, i) => {
			const t_i = paths[0]![i]!.t;
			const sum = paths.reduce((acc, p) => acc + (p[i]?.value ?? 0), 0);
			return { t: t_i, value: sum / paths.length };
		});
	}, [paths]);
	const revealedMean =
		revealCount !== undefined ? meanPath.slice(0, revealCount + 1) : meanPath;

	return (
		<ChartFrame
			title={title}
			subtitle={subtitle}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={height}
			legend={[
				{ label: "Paths", color },
				{ label: "Mean", color: t.status.critical },
			]}
		>
			<div ref={ref} className="w-full">
				<svg width={w} height={height} role="img">
					<Group left={m.left} top={m.top}>
						<Line from={{ x: 0, y: 0 }} to={{ x: 0, y: ih }} stroke={t.axis} />
						{revealed?.map((path, i) => (
							<LinePath
								key={i}
								data={path}
								x={(d) => xs(d.t)}
								y={(d) => ys(d.value)}
								stroke={color}
								strokeWidth={1}
								strokeOpacity={0.4}
							/>
						))}
						{revealedMean.length > 0 && (
							<LinePath
								data={revealedMean}
								x={(d) => xs(d.t)}
								y={(d) => ys(d.value)}
								stroke={t.status.critical}
								strokeWidth={2}
							/>
						)}
						<AxisBottom
							top={ih}
							scale={xs}
							numTicks={5}
							stroke={t.axis}
							tickStroke={t.axis}
							tickFormat={(v) => Number(v).toFixed(2)}
							tickLabelProps={() => ({
								fill: t.inkMuted,
								fontSize: 10,
								fontFamily: "var(--font-mono)",
							})}
						/>
						<AxisLeft
							scale={ys}
							numTicks={5}
							stroke={t.axis}
							tickStroke={t.axis}
							tickFormat={(v) => Number(v).toFixed(1)}
							tickLabelProps={() => ({
								fill: t.inkMuted,
								fontSize: 10,
								textAnchor: "end",
								dx: -2,
								fontFamily: "var(--font-mono)",
							})}
						/>
					</Group>
				</svg>
			</div>
		</ChartFrame>
	);
}
