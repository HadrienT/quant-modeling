import type { Meta, StoryObj } from "@storybook/react";

/**
 * Smoke story proving the Storybook harness works (WP 00 §6).
 * Real component stories arrive in WP 01.
 */
function Placeholder({ label }: { label: string }) {
	return (
		<div
			style={{
				padding: "1rem 1.25rem",
				border: "1px solid var(--color-hairline)",
				borderRadius: "var(--radius-md)",
				background: "var(--color-surface)",
				color: "var(--color-ink)",
				fontFamily: "var(--font-sans)",
			}}
		>
			{label}
		</div>
	);
}

const meta: Meta<typeof Placeholder> = {
	title: "Foundations/Placeholder",
	component: Placeholder,
	args: { label: "Storybook is wired up." },
};
export default meta;

type Story = StoryObj<typeof Placeholder>;
export const Default: Story = {};
