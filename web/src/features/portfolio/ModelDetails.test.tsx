import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import type { PositionMark } from "@/shared/api";
import * as fx from "@/shared/test/fixtures";
import { ModelDetails } from "./ModelDetails";

/**
 * What the i on a position shows. (The tooltip that wraps it is Radix's,
 * whose pointer and focus handling does not run in jsdom: opening it on
 * hover is checked in a real browser.)
 */
const [equity, option] = fx.portfolioSnapshot().positions as PositionMark[];

describe("ModelDetails", () => {
	it("names the model and every parameter with where it came from", () => {
		render(<ModelDetails position={option!} />);
		expect(
			screen.getByText("Black-Scholes, flat volatility"),
		).toBeInTheDocument();
		expect(screen.getByText("11.85 %")).toBeInTheDocument();
		expect(screen.getByText("proxied")).toBeInTheDocument();
		expect(
			screen.getByText("^FCHI realised vol, 63 business days"),
		).toBeInTheDocument();
		expect(screen.getByText("8,200")).toBeInTheDocument();
	});

	it("says an equity is marked at its close, with no model", () => {
		render(<ModelDetails position={equity!} />);
		expect(
			screen.getByText(/No model: marked at the stored close/),
		).toBeInTheDocument();
	});
});
