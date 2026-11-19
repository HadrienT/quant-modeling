import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { ComputeBreakdown } from "./compute-breakdown";

/** The legend entry whose text contains `label`. */
const entry = (label: string) =>
	screen.getAllByRole("listitem").find((li) => li.textContent?.includes(label));

const timings = {
	market_ms: 174,
	calibration_ms: 11.2,
	engine_ms: 23,
	other_ms: 0.7,
	greeks_ms: 0,
};

describe("ComputeBreakdown", () => {
	it("names the Monte-Carlo phase and gives every phase's time", () => {
		render(<ComputeBreakdown timings={timings} engine="mc" />);
		expect(entry("Market data")).toHaveTextContent("174 ms");
		expect(entry("Monte Carlo")).toHaveTextContent("23.0 ms");
		expect(screen.getByRole("img")).toHaveAccessibleName(/Server time 209 ms/);
	});

	it("lists the engine even when it took no measurable time", () => {
		render(
			<ComputeBreakdown
				timings={{ ...timings, calibration_ms: 0, engine_ms: 0.01 }}
				engine="analytic"
			/>,
		);
		expect(screen.getByText("Closed form")).toBeInTheDocument();
		expect(screen.queryByText("Calibration")).not.toBeInTheDocument();
	});

	it("shows the greeks by bump apart, and nothing without timings", () => {
		const { container, rerender } = render(
			<ComputeBreakdown timings={{ ...timings, greeks_ms: 40 }} />,
		);
		expect(entry("+ greeks by bump")).toHaveTextContent("40.0 ms");
		rerender(<ComputeBreakdown timings={null} />);
		expect(container).toBeEmptyDOMElement();
	});
});
