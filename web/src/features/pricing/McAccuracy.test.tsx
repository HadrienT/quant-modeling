import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import type { PricingResult } from "@/shared/api";
import { pricingResponse } from "@/shared/test/fixtures";
import { McAccuracy } from "./McAccuracy";

describe("McAccuracy", () => {
	it("says first when the time limit cut the run short", () => {
		const r = pricingResponse({
			warnings: [
				{
					code: "time_limit_paths",
					severity: "warning" as const,
					message:
						"Not the paths asked: priced with 1,250,000 of the 5,000,000 paths",
				},
				{
					code: "barrier_monitoring",
					severity: "info" as const,
					message: "other",
				},
			],
		}) as PricingResult;
		render(<McAccuracy r={r} />);
		expect(screen.getByRole("alert").textContent).toContain(
			"1,250,000 of the 5,000,000",
		);
		expect(screen.queryByText("other")).toBeNull();
	});

	it("shows no warning for a run the limit did not cut", () => {
		render(
			<McAccuracy r={pricingResponse({ warnings: [] }) as PricingResult} />,
		);
		expect(screen.queryByRole("alert")).toBeNull();
	});
});
