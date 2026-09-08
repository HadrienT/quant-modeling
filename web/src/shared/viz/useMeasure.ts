import { useLayoutEffect, useRef, useState } from "react";

/** Track an element's content-box size for responsive SVG charts. */
export function useMeasure<T extends HTMLElement>() {
	const ref = useRef<T>(null);
	const [size, setSize] = useState({ width: 0, height: 0 });

	useLayoutEffect(() => {
		const el = ref.current;
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
	}, []);

	return [ref, size] as const;
}
