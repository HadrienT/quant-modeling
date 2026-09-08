import { render } from "@testing-library/react";
import { axe } from "jest-axe";
import { describe, expect, it } from "vitest";
import { ApiError } from "@/shared/api/errors";
import { Button } from "./button";
import { Field } from "./field";
import {
	DeltaBadge,
	Metric,
	MetricRow,
	NumberCell,
	Uncertainty,
} from "./density";
import { EmptyState, ErrorState } from "./states";

/** jest-axe on the significant primitives — blueprint WP 12 §4. */
describe("accessibility", () => {
	it("numeric-density primitives have no axe violations", async () => {
		const { container } = render(
			<MetricRow>
				<Metric
					label="NPV"
					value={<NumberCell value={128.42} magnitude="price" />}
					change={<DeltaBadge value={2.1} />}
				/>
				<Metric label="MC" value={<Uncertainty value={128} stdError={0.3} />} />
			</MetricRow>,
		);
		expect(await axe(container)).toHaveNoViolations();
	});

	it("a labelled Field has no axe violations", async () => {
		const { container } = render(
			<Field label="Spot" type="number" defaultValue={100} />,
		);
		expect(await axe(container)).toHaveNoViolations();
	});

	it("empty and error states have no axe violations", async () => {
		const { container } = render(
			<div>
				<EmptyState title="Nothing yet" description="Add a position." />
				<ErrorState
					error={
						new ApiError({
							kind: "server",
							status: 500,
							code: "x",
							message: "boom",
						})
					}
					onRetry={() => {}}
				/>
				<Button>Retry</Button>
			</div>,
		);
		expect(await axe(container)).toHaveNoViolations();
	});
});
