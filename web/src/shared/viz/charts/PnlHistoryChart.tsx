import { useCallback, useEffect, useState } from "react";
import {
	type Time,
	BaselineSeries,
	HistogramSeries,
	createChart,
} from "lightweight-charts";
import { ChartFrame } from "../ChartFrame";
import { useChartTheme } from "../theme";
import { baseOptions, fitToContainer } from "./lightweightBase";

export type PnlPoint = {
	time: string;
	daily: number | null;
	cumulative: number | null;
};

/**
 * A portfolio's P&L through time: the cumulative P&L as a baseline at zero
 * (green above, red below) and each day's P&L as bars on their own scale
 * underneath. Days with no valuation are gaps, not zeros.
 */
export function PnlHistoryChart({
	points,
	currency,
	isLoading,
	error,
	height = 360,
}: {
	points?: PnlPoint[];
	currency: string;
	isLoading?: boolean;
	error?: unknown;
	height?: number;
}) {
	const t = useChartTheme();
	const [el, setEl] = useState<HTMLDivElement | null>(null);
	const setContainer = useCallback(
		(node: HTMLDivElement | null) => setEl(node),
		[],
	);

	useEffect(() => {
		if (!el || !points?.length) return;
		const c = createChart(el, { ...baseOptions(), height });

		const cumulative = c.addSeries(BaselineSeries, {
			baseValue: { type: "price", price: 0 },
			topLineColor: t.pnl.up,
			topFillColor1: "rgba(12,163,12,0.22)",
			topFillColor2: "rgba(12,163,12,0.02)",
			bottomLineColor: t.pnl.down,
			bottomFillColor1: "rgba(208,59,59,0.02)",
			bottomFillColor2: "rgba(208,59,59,0.22)",
			lineWidth: 2,
		});
		cumulative.setData(
			points
				.filter((p) => p.cumulative != null)
				.map((p) => ({ time: p.time as Time, value: p.cumulative! })),
		);
		c.priceScale("right").applyOptions({
			scaleMargins: { top: 0.08, bottom: 0.3 },
		});

		const daily = c.addSeries(HistogramSeries, { priceScaleId: "daily" });
		c.priceScale("daily").applyOptions({
			scaleMargins: { top: 0.75, bottom: 0 },
		});
		daily.setData(
			points
				.filter((p) => p.daily != null)
				.map((p) => ({
					time: p.time as Time,
					value: p.daily!,
					color: p.daily! >= 0 ? t.pnl.up : t.pnl.down,
				})),
		);

		const stopFit = fitToContainer(c, el);
		c.timeScale().fitContent();
		return () => {
			stopFit();
			c.remove();
		};
	}, [el, points, height, t]);

	return (
		<ChartFrame
			title="P&L history"
			subtitle={`cumulative (area) and daily (bars), ${currency}`}
			isLoading={isLoading}
			error={error}
			isEmpty={!points?.length}
			height={height}
			legend={[
				{ label: "Gain", color: t.pnl.up },
				{ label: "Loss", color: t.pnl.down },
			]}
			table={
				points
					? {
							columns: ["Date", "Daily P&L", "Cumulative P&L"],
							rows: points.map((p) => [
								p.time,
								p.daily?.toFixed(2) ?? "—",
								p.cumulative?.toFixed(2) ?? "—",
							]),
						}
					: undefined
			}
		>
			<div ref={setContainer} style={{ height, width: "100%" }} />
		</ChartFrame>
	);
}
