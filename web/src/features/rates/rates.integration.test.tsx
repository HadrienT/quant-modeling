import { QueryClientProvider } from "@tanstack/react-query";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { server } from "@/shared/test/msw-server";
import { TooltipProvider } from "@/shared/ui";
import fixtures from "./fixtures.test.json";
import RatesPage from "./RatesPage";

/**
 * Rates page against a real analysis (fixtures.test.json, dumped from the
 * API): the quotes are flagged as manual input, every model's price is shown
 * with its implied normal vol, an edited quote is sent only on "Recompute",
 * and the USD SOFR quotes built from traded prices come with the trades
 * behind them.
 */

function mountPage() {
	const bodies: unknown[] = [];
	server.use(
		http.get("*/api/rates/example", () => HttpResponse.json(fixtures.example)),
		http.get("*/api/rates/market", () => HttpResponse.json(fixtures.market)),
		http.post("*/api/rates/analyse", async ({ request }) => {
			bodies.push(await request.json());
			return HttpResponse.json(fixtures.analysis);
		}),
	);
	render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<RatesPage />
			</TooltipProvider>
		</QueryClientProvider>,
	);
	return bodies;
}

describe("RatesPage", () => {
	it("says the quotes are not market data and prices every model", async () => {
		mountPage();
		expect(await screen.findByText("Manual input")).toBeInTheDocument();
		expect(screen.getByText(/not market data/)).toBeInTheDocument();
		for (const model of [
			"Bachelier (normal)",
			"Hull-White (calibrated)",
			"SABR (shifted, Hagan 2002)",
		])
			expect(await screen.findByText(model)).toBeInTheDocument();
		expect(screen.getByText("Hull-White calibration")).toBeInTheDocument();
		expect(screen.getByText("Switch premium")).toBeInTheDocument();
	});

	it("sends an edited quote only when asked to recompute", async () => {
		const bodies = mountPage();
		await screen.findByText("Hull-White (calibrated)");
		expect(bodies).toHaveLength(1);
		const recompute = screen.getByRole("button", { name: "Recompute" });
		expect(recompute).toBeDisabled();
		fireEvent.change(screen.getByLabelText("Par OIS swaps row 1 Rate"), {
			target: { value: "2.5" },
		});
		expect(bodies).toHaveLength(1);
		fireEvent.click(recompute);
		await waitFor(() => expect(bodies).toHaveLength(2));
		const sent = bodies[1] as { ois: { rate: number }[] };
		expect(sent.ois[0]!.rate).toBeCloseTo(0.025, 12);
	});

	it("loads the quotes built from traded prices, with their trade counts", async () => {
		const bodies = mountPage();
		await screen.findByText("Hull-White (calibrated)");
		fireEvent.click(
			screen.getByRole("button", {
				name: "Use USD SOFR quotes from traded prices",
			}),
		);
		expect(await screen.findByText("Traded prices")).toBeInTheDocument();
		expect(screen.queryByText("Manual input")).not.toBeInTheDocument();
		expect(screen.getByText(/not dealer quotes/)).toBeInTheDocument();
		// The analysis is recomputed on the market quotes: one curve, annual legs.
		await waitFor(() => expect(bodies).toHaveLength(2));
		const sent = bodies[1] as { float_frequency: number; fras: unknown[] };
		expect(sent.float_frequency).toBe(1);
		expect(sent.fras).toHaveLength(0);
		// Every point says how many trades it rests on, and what was left out.
		expect(screen.getByText("Trades behind the quotes")).toBeInTheDocument();
		expect(screen.getByText(/168 swaption trades used/)).toBeInTheDocument();
		expect(screen.getByText(/away from the money/)).toBeInTheDocument();
		fireEvent.click(
			screen.getByRole("button", { name: "Use the illustrative EUR quotes" }),
		);
		expect(await screen.findByText("Manual input")).toBeInTheDocument();
	});

	it("says what is missing when the store has no market quotes", async () => {
		mountPage();
		server.use(
			http.get("*/api/rates/market", () =>
				HttpResponse.json(
					{
						code: "error",
						message:
							"The latest SOFR swap curve is from 2026-09-01, 30 days ago: data-ingest has not run, or DTCC has not published since",
					},
					{ status: 503 },
				),
			),
		);
		await screen.findByText("Hull-White (calibrated)");
		fireEvent.click(
			screen.getByRole("button", {
				name: "Use USD SOFR quotes from traded prices",
			}),
		);
		expect(await screen.findByRole("alert")).toHaveTextContent(/30 days ago/);
		// No silent fallback to the example: the user chooses to go back.
		expect(screen.queryByText("Manual input")).not.toBeInTheDocument();
	});
});
