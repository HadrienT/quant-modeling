import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { TooltipProvider } from "@/shared/ui";
import type * as Viz from "@/shared/viz";
import MarketPage from "./MarketPage";

// lightweight-charts draws on a <canvas>, which jsdom cannot provide: the
// price chart is replaced by its title, which is what these tests read.
vi.mock("@/shared/viz", async (importOriginal) => ({
	...(await importOriginal<typeof Viz>()),
	PriceSeriesChart: ({ title }: { title?: string }) => <div>{title}</div>,
}));

/**
 * Market first, then ticker, against the MSW fixtures: a non-US market opens
 * on its index, lists members by name, and its Volatility tab says why there
 * is no surface instead of failing.
 */
function renderMarket(search: string) {
	const root = createRootRoute();
	const market = createRoute({
		getParentRoute: () => root,
		path: "/market",
		validateSearch: (s: Record<string, unknown>) => s,
		component: MarketPage,
	});
	const router = createRouter({
		routeTree: root.addChildren([market]),
		history: createMemoryHistory({ initialEntries: [`/market${search}`] }),
	});
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<RouterProvider router={router as never} />
			</TooltipProvider>
		</QueryClientProvider>,
	);
}

describe("market page — markets", () => {
	it("offers the markets the API has, the current one pressed", async () => {
		renderMarket("?market=CAC40");
		const cac = await screen.findByRole("button", { name: "CAC 40" });
		expect(cac).toHaveAttribute("aria-pressed", "true");
		expect(screen.getByRole("button", { name: "S&P 500" })).toHaveAttribute(
			"aria-pressed",
			"false",
		);
	});

	it("opens a non-US market on its index, named", async () => {
		renderMarket("?market=CAC40");
		expect(
			await screen.findByText("^FCHI — CAC 40 (index)"),
		).toBeInTheDocument();
	});

	it("shows the price currency", async () => {
		renderMarket("?market=CAC40&ticker=MC.PA");
		expect(await screen.findByText("MC.PA — 1Y · EUR")).toBeInTheDocument();
	});

	it("gives an index level in points, not in a currency", async () => {
		renderMarket("?market=CAC40&ticker=%5EFCHI");
		expect(await screen.findByText("^FCHI — 1Y · pts")).toBeInTheDocument();
	});

	it("explains a market without option data instead of failing", async () => {
		renderMarket("?market=CAC40&tab=vol");
		expect(
			await screen.findByText(/publishes option chains for US listings only/),
		).toBeInTheDocument();
	});
});
