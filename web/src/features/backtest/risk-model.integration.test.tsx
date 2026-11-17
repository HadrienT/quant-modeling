import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { type BacktestTask, makeQueryClient } from "@/shared/api";
import fixture from "@/shared/test/risk-model.fixture.json";
import { RiskModelResults } from "./RiskModelResults";

/**
 * The risk-model tab against an answer recorded from the real API
 * (shared/test/risk-model.fixture.json, the multi-currency demo): every test
 * of the wheel shown with what its p-value can say.
 */

const data = (fixture as unknown as BacktestTask).risk_model!;

describe("risk model results", () => {
	it("shows the exceptions against the expected number and every test", () => {
		render(
			<QueryClientProvider client={makeQueryClient()}>
				<RiskModelResults data={data} />
			</QueryClientProvider>,
		);
		expect(
			screen.getByText(`${data.exceptions} of ${data.observations}`),
		).toBeInTheDocument();
		expect(
			screen.getByText(
				`${data.expected_exceptions.toFixed(1)} expected if the VaR is right.`,
			),
		).toBeInTheDocument();
		expect(screen.getByText(data.traffic_light.zone)).toBeInTheDocument();
		expect(screen.getByText(/little power/)).toBeInTheDocument();
		expect(
			screen.getByText(/hypothetical P&L, not the actual one/),
		).toBeInTheDocument();
	});
});
