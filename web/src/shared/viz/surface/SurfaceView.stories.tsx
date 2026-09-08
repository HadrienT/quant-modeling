import type { Meta, StoryObj } from "@storybook/react";
import { makeGrid } from "./SurfaceGrid";
import { SurfaceView } from "./SurfaceView";

const meta: Meta = { title: "Viz/3D surface" };
export default meta;
type Story = StoryObj;

const strikes = Array.from({ length: 21 }, (_, i) => 120 + i * 8);
const maturities = [0.08, 0.17, 0.25, 0.5, 0.75, 1, 1.5, 2, 3];

function smile(holes: boolean) {
	return makeGrid(
		strikes,
		maturities,
		maturities.map((t) =>
			strikes.map((k) => {
				if (holes && t <= 0.17 && (k < 145 || k > 240)) return null;
				const m = Math.log(k / 190);
				return 0.2 + 0.35 * m * m - 0.05 * m + 0.02 * Math.sqrt(t);
			}),
		),
		{
			x: { label: "Strike", format: (v) => v.toFixed(0) },
			y: {
				label: "Maturity",
				format: (v) => (v < 1 ? `${Math.round(v * 12)}m` : `${v}y`),
			},
			z: { label: "Implied vol", format: (v) => `${(v * 100).toFixed(1)}%` },
		},
	);
}

export const AnalyticSmile: Story = {
	render: () => <SurfaceView grid={smile(false)} title="Synthetic IV smile" />,
};

export const WithHoles: Story = {
	render: () => (
		<SurfaceView grid={smile(true)} title="Raw quotes — wings have no market" />
	),
};

export const Difference: Story = {
	render: () => (
		<SurfaceView
			grid={makeGrid(
				strikes,
				maturities,
				maturities.map((t) =>
					strikes.map((k) => 0.01 * Math.sin((k - 190) / 40) + 0.005 * (t - 1)),
				),
				{
					x: { label: "Strike" },
					y: { label: "Maturity" },
					z: {
						label: "Local − implied",
						format: (v) => `${(v * 100).toFixed(2)}%`,
					},
				},
			)}
			mode="divergent"
			title="Local vol − implied vol"
		/>
	),
};
