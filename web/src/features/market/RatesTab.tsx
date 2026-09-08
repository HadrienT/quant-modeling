import { useState } from "react";
import { useRatesCurve } from "@/shared/api";
import { RatesCurveChart } from "@/shared/viz";

const CURVES = ["Treasury", "SOFR", "FedFunds"] as const;

export function RatesTab() {
	const [curve, setCurve] = useState<(typeof CURVES)[number]>("Treasury");
	const zero = useRatesCurve(curve, "zero");
	const forward = useRatesCurve(curve, "forward", 0.5);

	const series = [
		zero.data && {
			label: "Zero",
			markers: true,
			points: zero.data.zero.map((p) => ({ x: p.x, y: p.y })),
		},
		forward.data && {
			label: "Forward (6m)",
			points: forward.data.zero.map((p) => ({ x: p.x, y: p.y })),
		},
	].filter(Boolean) as { label: string; points: { x: number; y: number }[] }[];

	// spread in a linked panel — never a second y-axis
	const spread =
		zero.data && forward.data
			? zero.data.zero.map((p, i) => ({
					x: p.x,
					y: (forward.data!.zero[i]?.y ?? p.y) - p.y,
				}))
			: [];

	return (
		<div className="flex flex-col gap-4">
			<div className="flex gap-1">
				{CURVES.map((c) => (
					<button
						key={c}
						type="button"
						onClick={() => setCurve(c)}
						className={
							"rounded-sm border px-2 py-1 text-xs " +
							(curve === c
								? "border-accent text-ink"
								: "border-hairline text-ink-secondary")
						}
					>
						{c}
					</button>
				))}
			</div>

			<RatesCurveChart
				title={`${curve} — zero vs forward`}
				isLoading={zero.isLoading}
				error={zero.error}
				series={series}
			/>

			{spread.length > 0 && (
				<RatesCurveChart
					title="Forward − zero spread"
					series={[{ label: "spread", points: spread }]}
				/>
			)}
		</div>
	);
}
