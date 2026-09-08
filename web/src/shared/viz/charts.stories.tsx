import type { Meta, StoryObj } from "@storybook/react";
import {
	AllocationChart,
	ConvergenceChart,
	DistributionChart,
	GreekProfileChart,
	PayoffChart,
	RatesCurveChart,
	SmileChart,
	StressMatrix,
	SurfaceHeatmap,
} from "./index";

const meta: Meta = { title: "Viz/2D charts" };
export default meta;
type Story = StoryObj;

const spot = Array.from({ length: 61 }, (_, i) => 140 + i * 2);

export const Payoff: Story = {
	render: () => (
		<PayoffChart
			data={{
				spot,
				atMaturity: spot.map((s) => Math.max(0, s - 185) - 6),
				atT: spot.map((s) => Math.max(0, s - 185) * 0.7 - 4),
				breakevens: [191],
				strikes: [185],
				currentSpot: 185,
			}}
		/>
	),
};

export const Greeks: Story = {
	render: () => (
		<GreekProfileChart
			currentSpot={185}
			profiles={["Delta", "Gamma", "Vega", "Theta", "Rho"].map((name, k) => ({
				name,
				spot,
				values: spot.map((s) => Math.sin((s - 140) / 30 + k) * (k + 1) * 0.1),
			}))}
		/>
	),
};

export const Convergence: Story = {
	render: () => (
		<ConvergenceChart
			series={[
				{
					label: "pseudo-random",
					points: [1e3, 4e3, 16e3, 64e3, 256e3, 1e6].map((n) => ({
						paths: n,
						error: 0.5 / Math.sqrt(n),
					})),
				},
				{
					label: "Sobol (QMC)",
					points: [1e3, 4e3, 16e3, 64e3, 256e3, 1e6].map((n) => ({
						paths: n,
						error: 0.5 / n ** 0.9,
					})),
				},
			]}
		/>
	),
};

export const Allocation: Story = {
	render: () => (
		<AllocationChart
			rows={[
				{ label: "AAPL", weight: 0.34, returnPct: 592 },
				{ label: "MSFT", weight: 0.28, returnPct: 802 },
				{ label: "XOM", weight: 0.22, returnPct: 12 },
				{ label: "JPM", weight: 0.16, returnPct: 174 },
			]}
		/>
	),
};

export const Distribution: Story = {
	render: () => (
		<DistributionChart
			samples={Array.from(
				{ length: 4000 },
				() => (Math.random() + Math.random() + Math.random() - 1.5) * 8000,
			)}
			varLevel={-9000}
			expectedShortfall={-13500}
		/>
	),
};

export const Rates: Story = {
	render: () => (
		<RatesCurveChart
			series={[
				{
					label: "Zero",
					markers: true,
					points: [0.25, 1, 2, 5, 10, 30].map((x) => ({
						x,
						y: 0.04 + 0.005 * Math.log(1 + x),
					})),
				},
				{
					label: "Forward",
					points: [0.25, 1, 2, 5, 10, 30].map((x) => ({
						x,
						y: 0.045 + 0.004 * Math.log(1 + x),
					})),
				},
			]}
		/>
	),
};

export const Smile: Story = {
	render: () => (
		<SmileChart
			slices={[
				{
					label: "1M",
					x: [150, 160, 170, 180, 190, 200, 210],
					iv: [0.3, null, 0.24, 0.22, 0.23, 0.26, 0.31],
				},
				{
					label: "6M",
					x: [150, 160, 170, 180, 190, 200, 210],
					iv: [0.27, 0.25, 0.23, 0.22, 0.225, 0.24, 0.27],
				},
			]}
		/>
	),
};

export const Heatmap: Story = {
	render: () => (
		<SurfaceHeatmap
			grid={{
				x: [150, 160, 170, 180, 190, 200, 210],
				y: [0.08, 0.25, 0.5, 1, 2],
				z: [0.08, 0.25, 0.5, 1, 2].map((t) =>
					[150, 160, 170, 180, 190, 200, 210].map((k) =>
						t < 0.25 && (k < 160 || k > 200)
							? null
							: 0.22 + 0.001 * Math.abs(k - 185) + 0.01 * Math.sqrt(t),
					),
				),
			}}
		/>
	),
};

export const Stress: Story = {
	render: () => (
		<StressMatrix
			rows={["+20%", "+10%", "0", "−10%", "−20%"]}
			cols={["−10v", "−5v", "0", "+5v", "+10v"]}
			cells={["+20%", "+10%", "0", "−10%", "−20%"].flatMap((r, ri) =>
				["−10v", "−5v", "0", "+5v", "+10v"].map((c, ci) => ({
					row: r,
					col: c,
					value: (2 - ri) * 4000 + (ci - 2) * 1500,
				})),
			)}
		/>
	),
};
