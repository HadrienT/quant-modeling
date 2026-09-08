import type { Meta, StoryObj } from "@storybook/react";
import { ApiError } from "@/shared/api/errors";
import { Button } from "./button";
import {
	ChartSkeleton,
	EmptyState,
	ErrorState,
	MetricRowSkeleton,
	TableSkeleton,
} from "./states";

const meta: Meta = { title: "Shell/States" };
export default meta;
type Story = StoryObj;

export const Loading: Story = {
	render: () => (
		<div className="flex flex-col gap-6">
			<MetricRowSkeleton />
			<ChartSkeleton />
			<TableSkeleton rows={5} />
		</div>
	),
};

export const Empty: Story = {
	render: () => (
		<div className="flex flex-col gap-4">
			<EmptyState
				kind="no-results"
				title="No positions match this filter"
				description="Clear the category filter to see the full book."
				action={<Button size="sm">Clear filter</Button>}
			/>
			<EmptyState
				kind="sign-in"
				title="Sign in to load server-side portfolios"
				description="Anonymous portfolios stay on this device."
			/>
		</div>
	),
};

export const Errored: Story = {
	render: () => (
		<ErrorState
			error={
				new ApiError({
					kind: "server",
					status: 500,
					code: "engine_failure",
					message: "The pricing engine returned no result.",
					detail: { diagnostics: "mc: NaN in path 41221" },
				})
			}
			onRetry={() => {}}
		/>
	),
};
