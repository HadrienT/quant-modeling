import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import {
	DeltaBadge,
	DiscretisationBias,
	Freshness,
	Provenance,
	Uncertainty,
} from "./density";

describe("Uncertainty", () => {
	it("keeps value and standard error together", () => {
		render(<Uncertainty value={12.3456} stdError={0.0021} magnitude="price" />);
		expect(screen.getByText(/12\.3456/)).toHaveTextContent("± 0.0021");
	});

	it("switches to a warning when the relative error is large", () => {
		render(<Uncertainty value={1} stdError={0.2} magnitude="price" />);
		expect(screen.getByTitle(/Relative standard error/)).toBeInTheDocument();
	});

	it("renders a dash without a value", () => {
		render(<Uncertainty value={null} stdError={null} />);
		expect(screen.getByText("—")).toBeInTheDocument();
	});
});

describe("DeltaBadge", () => {
	it("shows an up arrow and explicit plus for gains", () => {
		render(<DeltaBadge value={4.2} />);
		expect(screen.getByText("▲")).toBeInTheDocument();
		expect(screen.getByText(/\+4\.20/)).toBeInTheDocument();
	});
	it("shows a down arrow and minus for losses", () => {
		render(<DeltaBadge value={-4.2} />);
		expect(screen.getByText("▼")).toBeInTheDocument();
		expect(screen.getByText(/−4\.20/)).toBeInTheDocument();
	});
});

describe("Freshness", () => {
	it("greys and warns past the staleness threshold", () => {
		const now = Date.parse("2026-01-01T00:10:00Z");
		render(
			<Freshness at="2026-01-01T00:00:00Z" staleAfterSeconds={300} now={now} />,
		);
		expect(screen.getByText("⚠")).toBeInTheDocument();
	});
});

describe("Provenance", () => {
	it("labels a market value", () => {
		render(<Provenance source="market" />);
		expect(screen.getByText("market")).toBeInTheDocument();
	});
});

describe("DiscretisationBias", () => {
	it("shows the bias and says the standard error leaves it out", () => {
		render(<DiscretisationBias bias={0.13} value={54.2} />);
		expect(screen.getByText(/Time-step bias/)).toHaveTextContent(
			/0\.13.*0\.24 %.*not in the ±/,
		);
	});

	it("says an exact simulation has none", () => {
		render(<DiscretisationBias bias={0} value={10} />);
		expect(screen.getByText(/exact simulation/)).toBeInTheDocument();
	});

	it("renders nothing when not estimated", () => {
		const { container } = render(<DiscretisationBias bias={null} value={10} />);
		expect(container).toBeEmptyDOMElement();
	});
});
