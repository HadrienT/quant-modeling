import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, within } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { describe, expect, it, vi } from "vitest";
import { type RateCurrency, makeQueryClient } from "@/shared/api";
import { server } from "@/shared/test/msw-server";
import { TooltipProvider } from "@/shared/ui";
import type * as Viz from "@/shared/viz";
import { RatesTab } from "./RatesTab";

// lightweight-charts draws on a <canvas>, which jsdom cannot provide: the
// history chart of the reference rates is replaced by its title.
vi.mock("@/shared/viz", async (importOriginal) => ({
	...(await importOriginal<typeof Viz>()),
	PriceSeriesChart: ({ title }: { title?: string }) => <div>{title}</div>,
}));

/**
 * Rates tab against the MSW fixtures: what the page must never do is draw a
 * curve it cannot defend, or hide how a number was made.
 */
function renderRates(currency: RateCurrency) {
	// The swap market panel links to the pricing workbench: it needs a router.
	const root = createRootRoute();
	const routes = ["/market", "/price"].map((path) =>
		createRoute({
			getParentRoute: () => root,
			path,
			validateSearch: (s: Record<string, unknown>) => s,
			component: () => <RatesTab currency={currency} onCurrency={() => {}} />,
		}),
	);
	const router = createRouter({
		routeTree: root.addChildren(routes),
		history: createMemoryHistory({ initialEntries: ["/market"] }),
	});
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<RouterProvider router={router as never} />
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

	it("shows the USD swap curve and swaption vols with the trades behind them", async () => {
		renderRates("USD");
		expect(
			await screen.findByRole("heading", {
				name: "SOFR swaps and swaptions, as traded",
			}),
		).toBeInTheDocument();
		expect(await screen.findByText("Traded prices")).toBeInTheDocument();
		expect(screen.getByText(/Medians of trades/)).toBeInTheDocument();
		const vols = screen.getByRole("table", {
			name: /ATM swaption normal vols/,
		});
		expect(within(vols).getAllByText(/into/).length).toBeGreaterThan(3);
		expect(
			screen.getByText(/swaption trades left out: away from the money/),
		).toBeInTheDocument();
		expect(
			screen.getByRole("link", { name: /Price a swap or a swaption/ }),
		).toHaveAttribute("href", "/price?product=swap");
	});

	it("has no swap market section where no trades are published", async () => {
		renderRates("EUR");
		await screen.findByText("Past average");
		expect(
			screen.queryByText("SOFR swaps and swaptions, as traded"),
		).not.toBeInTheDocument();
	});

	it("says what the store is missing when the trades are stale", async () => {
		server.use(
			http.get("*/api/rates/quotes/usd-sofr", () =>
				HttpResponse.json(
					{ code: "error", message: "No SOFR swap curve in the store" },
					{ status: 503 },
				),
			),
		);
		renderRates("USD");
		expect(await screen.findByRole("alert")).toHaveTextContent(
			"No SOFR swap curve in the store",
		);
		// The rest of the tab does not depend on it.
		expect(screen.getByText("USD government curve")).toBeInTheDocument();
	});
});
