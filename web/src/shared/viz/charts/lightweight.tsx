import { useEffect, useRef } from "react";
import {
	type IChartApi,
	type ISeriesApi,
	type Time,
	AreaSeries,
	CandlestickSeries,
	ColorType,
	HistogramSeries,
	LineSeries,
	createChart,
} from "lightweight-charts";
import { ChartFrame } from "../ChartFrame";
import { chartTheme, useChartTheme } from "../theme";

/**
 * lightweight-charts wrappers (blueprint WP 06 §2, ADR-003). Canvas, tens of
 * thousands of points, native pan/zoom/crosshair. The engine has its own theme
 * system — tokens are PUSHED on every theme change (pitfalls).
 */

export type Candle = {
	time: string;
	open: number;
	high: number;
	low: number;
	close: number;
	volume?: number;
};
export type LinePoint = { time: string; value: number };

function baseOptions() {
	const t = chartTheme();
	return {
		layout: {
			background: { type: ColorType.Solid, color: "transparent" },
			textColor: t.inkMuted,
			fontFamily: "var(--font-mono)",
			fontSize: 10,
		},
		grid: {
			vertLines: { color: t.hairline },
			horzLines: { color: t.hairline },
		},
		rightPriceScale: { borderColor: t.axis },
		timeScale: { borderColor: t.axis },
		crosshair: { mode: 1 as const },
		// Explicit sizing (see `fitToContainer`) rather than autoSize: autoSize
		// occasionally leaves a fresh mount at 0×0 until an unrelated resize.
		autoSize: false,
	};
}

/** Size a chart to its container now, and keep it in sync on resize. Returns a cleanup. */
function fitToContainer(chart: IChartApi, container: HTMLElement): () => void {
	const apply = () => {
		const { clientWidth, clientHeight } = container;
		if (clientWidth > 0) {
			chart.resize(clientWidth, clientHeight || 300);
		}
	};
	apply();
	const ro = new ResizeObserver(apply);
	ro.observe(container);
	// one more tick in case layout isn't settled on the mount frame
	const raf = requestAnimationFrame(apply);
	return () => {
		ro.disconnect();
		cancelAnimationFrame(raf);
	};
}

/* ── Price tape ────────────────────────────────────────────────────────── */
export function PriceSeriesChart({
	candles,
	kind = "line",
	title = "Price",
	isLoading,
	error,
	height = 320,
}: {
	candles?: Candle[];
	kind?: "line" | "candles";
	title?: string;
	isLoading?: boolean;
	error?: unknown;
	height?: number;
}) {
	const t = useChartTheme();
	const el = useRef<HTMLDivElement>(null);
	const chart = useRef<IChartApi>(null);

	useEffect(() => {
		if (!el.current || !candles?.length) return;
		const c = createChart(el.current, { ...baseOptions(), height });
		chart.current = c;

		const price =
			kind === "candles"
				? c.addSeries(CandlestickSeries, {
						upColor: t.pnl.up,
						downColor: t.pnl.down,
						wickUpColor: t.pnl.up,
						wickDownColor: t.pnl.down,
						borderVisible: false,
					})
				: c.addSeries(LineSeries, { color: t.series[0], lineWidth: 2 });

		if (kind === "candles") {
			(price as ISeriesApi<"Candlestick">).setData(
				candles.map((d) => ({
					time: d.time as Time,
					open: d.open,
					high: d.high,
					low: d.low,
					close: d.close,
				})),
			);
		} else {
			(price as ISeriesApi<"Line">).setData(
				candles.map((d) => ({ time: d.time as Time, value: d.close })),
			);
		}

		if (candles.some((d) => d.volume != null)) {
			const vol = c.addSeries(HistogramSeries, {
				priceScaleId: "vol",
				color: t.hairline,
				priceFormat: { type: "volume" },
			});
			c.priceScale("vol").applyOptions({
				scaleMargins: { top: 0.82, bottom: 0 },
			});
			vol.setData(
				candles.map((d) => ({
					time: d.time as Time,
					value: d.volume ?? 0,
				})),
			);
		}

		const stopFit = fitToContainer(c, el.current);
		c.timeScale().fitContent();
		return () => {
			stopFit();
			c.remove();
		};
	}, [candles, kind, height, t]);

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={!candles?.length}
			height={height}
			table={
				candles
					? {
							columns: ["Date", "Close"],
							rows: candles.map((d) => [d.time, d.close.toFixed(2)]),
						}
					: undefined
			}
		>
			<div ref={el} style={{ height, width: "100%" }} />
		</ChartFrame>
	);
}

/* ── Equity curve (base 100) + drawdown, crosshair-synced ──────────────── */
export function EquityAndDrawdownChart({
	portfolio,
	benchmark,
	benchmarkLabel = "S&P 500",
	optWindow,
	title = "Equity curve vs benchmark",
	isLoading,
	error,
}: {
	portfolio?: LinePoint[];
	benchmark?: LinePoint[] | null;
	benchmarkLabel?: string;
	optWindow?: { start: string; end: string };
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const topEl = useRef<HTMLDivElement>(null);
	const botEl = useRef<HTMLDivElement>(null);

	useEffect(() => {
		if (!topEl.current || !botEl.current || !portfolio?.length) return;

		const index100 = (s: LinePoint[]) => {
			const base = s[0]?.value || 1;
			return s.map((p) => ({
				time: p.time as Time,
				value: (p.value / base) * 100,
			}));
		};
		const drawdown = (s: LinePoint[]) => {
			let peak = -Infinity;
			return s.map((p) => {
				peak = Math.max(peak, p.value);
				return { time: p.time as Time, value: (p.value / peak - 1) * 100 };
			});
		};

		const top = createChart(topEl.current, { ...baseOptions(), height: 260 });
		const bot = createChart(botEl.current, { ...baseOptions(), height: 120 });

		const pLine = top.addSeries(LineSeries, {
			color: t.series[0],
			lineWidth: 2,
		});
		pLine.setData(index100(portfolio));

		if (benchmark?.length) {
			const bLine = top.addSeries(LineSeries, {
				color: t.series[1],
				lineWidth: 2,
			});
			bLine.setData(index100(benchmark));
		}

		const dd = bot.addSeries(AreaSeries, {
			lineColor: t.status.critical,
			topColor: "rgba(208,59,59,0.25)",
			bottomColor: "rgba(208,59,59,0.02)",
			lineWidth: 1,
		});
		dd.setData(drawdown(portfolio));

		// crosshair + time-range sync
		const sync = (from: IChartApi, to: IChartApi) => {
			from.timeScale().subscribeVisibleLogicalRangeChange((r) => {
				if (r) to.timeScale().setVisibleLogicalRange(r);
			});
		};
		sync(top, bot);
		sync(bot, top);
		const stopTop = fitToContainer(top, topEl.current);
		const stopBot = fitToContainer(bot, botEl.current);
		top.timeScale().fitContent();

		return () => {
			stopTop();
			stopBot();
			top.remove();
			bot.remove();
		};
	}, [portfolio, benchmark, t]);

	return (
		<ChartFrame
			title={title}
			subtitle={
				optWindow
					? `grey band = in-sample optimisation window (${optWindow.start} – ${optWindow.end})`
					: "indexed to base 100"
			}
			isLoading={isLoading}
			error={error}
			isEmpty={!portfolio?.length}
			height={400}
			legend={[
				{ label: "Portfolio", color: t.series[0]! },
				...(benchmark?.length
					? [{ label: benchmarkLabel, color: t.series[1]! }]
					: []),
				{ label: "Drawdown", color: t.status.critical },
			]}
		>
			<div className="flex flex-col gap-1">
				<div ref={topEl} style={{ height: 260, width: "100%" }} />
				<div ref={botEl} style={{ height: 120, width: "100%" }} />
			</div>
		</ChartFrame>
	);
}
