import { LinePath, Line } from "@visx/shape";
import { scaleLog } from "@visx/scale";
import { Group } from "@visx/group";
import { AxisBottom, AxisLeft } from "@visx/axis";
import { ChartFrame } from "../ChartFrame";
import { useChartTheme } from "../theme";
import { useMeasure } from "../useMeasure";

export type ConvergencePoint = { paths: number; error: number };

/**
 * MC error vs number of paths on log–log axes, with the theoretical 1/√N
 * reference slope drawn (blueprint WP 06 §2). This is the chart that shows the
 * estimator behaves — and that QMC beats it.
 */
export function ConvergenceChart({
	series,
	title = "Monte-Carlo convergence",
	isLoading,
	error,
}: {
	series?: { label: string; points: ConvergencePoint[]; color?: string }[];
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const [ref, { width }] = useMeasure<HTMLDivElement>();
	const w = width || 560;
	const height = 260;
	const m = { top: 10, right: 16, bottom: 30, left: 48 };
	const iw = Math.max(0, w - m.left - m.right);
	const ih = height - m.top - m.bottom;

	const all = series?.flatMap((s) => s.points) ?? [];
	const isEmpty = all.length === 0;

	const xs = scaleLog({
		domain: [
			Math.min(...all.map((p) => p.paths)) || 1e3,
			Math.max(...all.map((p) => p.paths)) || 1e6,
		],
		range: [0, iw],
	});
	const ys = scaleLog({
		domain: [
			Math.min(...all.map((p) => p.error)) || 1e-4,
			Math.max(...all.map((p) => p.error)) || 1e-1,
		],
		range: [ih, 0],
	});

	// 1/√N reference anchored at the first point.
	const anchor = series?.[0]?.points[0];
	const ref1 = anchor
		? all
				.map((p) => p.paths)
				.sort((a, b) => a - b)
				.map((n) => ({
					paths: n,
					error: anchor.error * Math.sqrt(anchor.paths / n),
				}))
		: [];

	return (
		<ChartFrame
			title={title}
			subtitle="log–log · reference slope ∝ 1/√N"
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={height}
			legend={[
				...(series?.map((s, i) => ({
					label: s.label,
					color: s.color ?? t.series[i % t.series.length]!,
				})) ?? []),
				{ label: "1/√N", color: t.inkMuted },
			]}
			table={
				series?.[0]
					? {
							columns: ["Paths", ...series.map((s) => s.label)],
							rows: series[0].points.map((p, i) => [
								p.paths,
								...series.map((s) =>
									(s.points[i]?.error ?? 0).toExponential(2),
								),
							]),
						}
					: undefined
			}
		>
			<div ref={ref} className="w-full">
				<svg width={w} height={height} role="img">
					<Group left={m.left} top={m.top}>
						<Line from={{ x: 0, y: 0 }} to={{ x: 0, y: ih }} stroke={t.axis} />
						{ref1.length > 0 && (
							<LinePath
								data={ref1}
								x={(d) => xs(d.paths)}
								y={(d) => ys(d.error)}
								stroke={t.inkMuted}
								strokeWidth={1}
								strokeDasharray="4 3"
							/>
						)}
						{series?.map((s, i) => (
							<LinePath
								key={s.label}
								data={s.points}
								x={(d) => xs(d.paths)}
								y={(d) => ys(d.error)}
								stroke={s.color ?? t.series[i % t.series.length]}
								strokeWidth={1.75}
							/>
						))}
						<AxisBottom
							top={ih}
							scale={xs}
							numTicks={4}
							stroke={t.axis}
							tickStroke={t.axis}
							tickFormat={(v) => {
								const n = Number(v);
								return n >= 1e6 ? `${n / 1e6}M` : `${n / 1e3}k`;
							}}
							tickLabelProps={() => ({
								fill: t.inkMuted,
								fontSize: 10,
								fontFamily: "var(--font-mono)",
							})}
						/>
						<AxisLeft
							scale={ys}
							numTicks={4}
							stroke={t.axis}
							tickStroke={t.axis}
							tickFormat={(v) => Number(v).toExponential(0)}
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
