import { type IChartApi, ColorType } from "lightweight-charts";
import { chartTheme } from "../theme";

/** Shared by the lightweight-charts wrappers: theme options and sizing. */
export function baseOptions() {
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
export function fitToContainer(
	chart: IChartApi,
	container: HTMLElement,
): () => void {
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
