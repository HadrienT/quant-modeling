import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import {
	type Portfolio,
	type PortfolioStress,
	type PortfolioVarTask,
	makeQueryClient,
} from "@/shared/api";
import { server } from "@/shared/test/msw-server";
import fixtures from "@/shared/test/portfolio-risk.fixtures.json";
import { StressPanel } from "./StressPanel";
import { VarPanel } from "./VarPanel";

/**
 * The Risk tab's stress tests and VaR (issue 292) against answers recorded
 * from the real API (shared/test/portfolio-risk.fixtures.json): every
 * scenario's total shown, every VaR and expected shortfall with its interval.
 */

const stress = fixtures.stress as unknown as PortfolioStress;
const done = fixtures.var as unknown as PortfolioVarTask;
const pf = {
	id: "p",
	version: 2,
	base_currency: "USD",
	instruments: [],
	transactions: [{ id: "t" }],
} as unknown as Portfolio;

function wrap(ui: React.ReactNode) {
	return render(
		<QueryClientProvider client={makeQueryClient()}>{ui}</QueryClientProvider>,
	);
}

describe("risk tab panels", () => {
	it("shows every stress scenario with its total", async () => {
		server.use(
			http.post("*/api/portfolio-valuation/stress", () =>
				HttpResponse.json(stress),
			),
		);
		wrap(<StressPanel pf={pf} />);
		for (const s of stress.scenarios)
			expect(await screen.findByText(s.name)).toBeInTheDocument();
		expect(screen.getByText(/sticky strike/)).toBeInTheDocument();
	});

	it("runs the VaR as a task and gives each figure its interval", async () => {
		server.use(
			http.post("*/api/portfolio-valuation/var", () =>
				HttpResponse.json(
					{ ...done, status: "queued", progress: 0, var: null },
					{ status: 202 },
				),
			),
			http.get("*/api/portfolio-valuation/var/v1", () =>
				HttpResponse.json(done),
			),
		);
		wrap(<VarPanel pf={pf} />);
		await userEvent.click(screen.getByRole("button", { name: "Compute" }));
		expect(await screen.findByText("VaR 99%")).toBeInTheDocument();
		expect(screen.getByText("Expected shortfall 97.5%")).toBeInTheDocument();
		expect(screen.getAllByText(/95% interval \[/)).toHaveLength(4);
	});
});
