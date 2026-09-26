import { render, screen, within } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { ValidationSummary } from "./ValidationSummary";

const analysis = {
	n_underlyings: 1,
	nonlinear_in_spot: false,
	spot_threshold_test: false,
	path_dependent: true,
};

/** 250 business-daily events over a year. */
const daily = Array.from({ length: 250 }, (_, i) => ({
	date: `d${i}`,
	t: i / 252,
}));

describe("ValidationSummary", () => {
	it("folds a daily schedule into one line instead of listing every date", () => {
		render(
			<ValidationSummary
				result={{ events: daily, variables: ["x"], analysis }}
			/>,
		);
		expect(screen.getByText(/250 event\(s\)/)).toBeInTheDocument();
		expect(screen.getByText(/\(daily\)/)).toBeInTheDocument();
		expect(screen.getByText("Show all 250 dates")).toBeInTheDocument();
		// the dates live inside the folded <details> (role "group")
		expect(
			within(screen.getByRole("group")).getByText("d100", { exact: false }),
		).toBeInTheDocument();
	});

	it("lists a short schedule inline", () => {
		const events = [
			{ date: "2027-01-04", t: 0.25 },
			{ date: "2027-04-05", t: 0.5 },
		];
		render(<ValidationSummary result={{ events, variables: [], analysis }} />);
		expect(screen.queryByText(/Show all/)).not.toBeInTheDocument();
		expect(screen.queryByRole("group")).not.toBeInTheDocument();
		// once in the "first → last" range, once in the inline list
		expect(screen.getAllByText(/2027-04-05/)).toHaveLength(2);
	});
});
