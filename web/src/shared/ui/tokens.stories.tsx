import type { Meta, StoryObj } from "@storybook/react";

const meta: Meta = { title: "Design system/Tokens" };
export default meta;
type Story = StoryObj;

function Swatch({ name, varName }: { name: string; varName: string }) {
	return (
		<div className="flex items-center gap-3">
			<span
				className="size-8 rounded-sm border border-hairline"
				style={{ background: `var(${varName})` }}
			/>
			<span className="font-mono text-xs text-ink-secondary">{varName}</span>
			<span className="text-xs text-ink-muted">{name}</span>
		</div>
	);
}

export const Surfaces: Story = {
	render: () => (
		<div className="flex flex-col gap-2">
			{[
				["Page", "--color-canvas"],
				["Surface", "--color-surface"],
				["Surface raised", "--color-surface-raised"],
				["Primary ink", "--color-ink"],
				["Secondary ink", "--color-ink-secondary"],
				["Muted ink", "--color-ink-muted"],
				["Hairline", "--color-hairline"],
				["Axis", "--color-axis"],
			].map(([n, v]) => (
				<Swatch key={v} name={n!} varName={v!} />
			))}
		</div>
	),
};

export const SeriesPalette: Story = {
	render: () => (
		<div className="flex flex-col gap-2">
			{[1, 2, 3, 4, 5, 6, 7, 8].map((i) => (
				<Swatch key={i} name={`series ${i}`} varName={`--color-series-${i}`} />
			))}
		</div>
	),
};

export const Scales: Story = {
	render: () => (
		<div className="flex flex-col gap-6">
			<div>
				<p className="mb-2 text-xs text-ink-secondary">
					Sequential — single hue (no rainbow)
				</p>
				<div className="flex">
					{[0, 1, 2, 3, 4].map((i) => (
						<span
							key={i}
							className="h-8 flex-1"
							style={{ background: `var(--color-seq-${i})` }}
						/>
					))}
				</div>
			</div>
			<div>
				<p className="mb-2 text-xs text-ink-secondary">
					Divergent — blue ↔ red, neutral grey at zero
				</p>
				<div className="flex">
					{["low", "mid", "high"].map((k) => (
						<span
							key={k}
							className="h-8 flex-1"
							style={{ background: `var(--color-diverge-${k})` }}
						/>
					))}
				</div>
			</div>
		</div>
	),
};
