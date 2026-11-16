import { useLayoutEffect, useState } from "react";

/**
 * Track an element's content-box size for responsive SVG charts.
 *
 * Returns a callback ref, not a ref object: a chart that first renders its
 * empty or loading state mounts the measured element only later, and an
 * effect keyed on a ref object would never see it, leaving the chart at its
 * fallback width for good.
 */
export function useMeasure<T extends HTMLElement>() {
	const [el, setEl] = useState<T | null>(null);
	const [size, setSize] = useState({ width: 0, height: 0 });

	useLayoutEffect(() => {
		if (!el) return;
		const ro = new ResizeObserver(([entry]) => {
			if (!entry) return;
			const { width, height } = entry.contentRect;
			setSize((prev) =>
				prev.width === width && prev.height === height
					? prev
					: { width, height },
			);
		});
		ro.observe(el);
		return () => ro.disconnect();
	}, [el]);

	return [setEl, size] as const;
}
