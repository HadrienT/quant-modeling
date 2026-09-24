import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { type RateCurrency, makeQueryClient } from "@/shared/api";
import { TooltipProvider } from "@/shared/ui";
import { RatesTab } from "./RatesTab";

/**
 * Rates tab against the MSW fixtures: what the page must never do is draw a
 * curve it cannot defend, or hide how a number was made.
 */
function renderRates(currency: RateCurrency) {
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<RatesTab currency={currency} onCurrency={() => {}} />
			</TooltipProvider>
		</QueryClientProvider>,
	);
}

describe("rates tab", () => {
	it("always shows the methodology, next to the curve", async () => {
		renderRates("USD");
		expect(
			await screen.findByRole("heading", { name: "Methodology" }),
		).toBeInTheDocument();
		expect(
			screen.getByRole("heading", { name: "Derived zero and forward curves" }),
		).toBeInTheDocument();
		expect(screen.getByText("USD government curve")).toBeInTheDocument();
	});

	it("labels published averages as backward-looking, not as term rates", async () => {
		renderRates("EUR");
		expect(await screen.findByText("Past average")).toBeInTheDocument();
		expect(screen.getByText("Overnight fixing")).toBeInTheDocument();
		expect(screen.getByText("3.850%")).toBeInTheDocument();
	});

	it("says why a currency has no government curve instead of drawing one", async () => {
		renderRates("CHF");
		expect(
			await screen.findByText("No free CHF government curve."),
		).toBeInTheDocument();
		expect(screen.queryByText("Term structure")).not.toBeInTheDocument();
	});

	it("shows only the quotes when no zero curve can be derived", async () => {
		renderRates("GBP");
		expect(
			await screen.findByText("Only three par yields."),
		).toBeInTheDocument();
		expect(screen.queryByText(/Lines are derived/)).not.toBeInTheDocument();
	});
});
