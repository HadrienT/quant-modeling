import { useMemo } from "react";
import type { XvaExposure } from "@/shared/api";
import { SurfaceView, makeGrid } from "@/shared/viz";

const COMPACT = new Intl.NumberFormat("en-US", {
	notation: "compact",
	maximumFractionDigits: 1,
});

/**
 * The distribution of the netting set's value through time, in 3D: along one
 * axis the dates, along the other the scenarios ranked from the worst for the
 * bank to the best (the quantile level), and the value as the height. The
 * profiles of the chart above are read off it: the PFE is one of its lines,
 * the expected exposure the average of what is above zero.
 */
export function ExposureSurface({ exposure }: { exposure: XvaExposure }) {
	const grid = useMemo(
		() => ({
			...makeGrid(
				exposure.times,
				exposure.quantile_levels.map((q) => q * 100),
				exposure.value_quantiles,
				{
					x: { label: "Years", format: (v) => `${Math.round(v * 100) / 100}y` },
					y: { label: "Scenario quantile", unit: "%", format: (v) => `${v} %` },
					z: {
						label: "Value of the netting set",
						format: (v) => (v < 0 ? "−" : "") + COMPACT.format(Math.abs(v)),
					},
				},
			),
			// Quantiles of a simulation, not noisy quotes: nothing to clip.
			displayQuantiles: [0, 1] as [number, number],
		}),
		[exposure],
	);
	if (exposure.quantile_levels.length < 2) return null;
	return (
		<SurfaceView
			grid={grid}
			mode="divergent"
			title="Value of the netting set: scenario × time"
			height={380}
		/>
	);
}
