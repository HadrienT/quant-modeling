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
 * with its implied normal vol, and an edited quote is sent only on
 * "Recompute".
 */

function mountPage() {
	const bodies: unknown[] = [];
	server.use(
		http.get("*/api/rates/example", () => HttpResponse.json(fixtures.example)),
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
});
