import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { TooltipProvider } from "@/shared/ui";
import PricingPage from "./PricingPage";

/**
 * Feature integration (WP 12 §1): the workbench prices against the MSW fixtures
 * and shows the standard error next to a Monte-Carlo price.
 */
function renderWorkbench(search = "?product=vanilla&engine=mc") {
	const root = createRootRoute();
	const price = createRoute({
		getParentRoute: () => root,
		path: "/price",
		validateSearch: (s: Record<string, unknown>) => s,
		component: PricingPage,
	});
	const router = createRouter({
		routeTree: root.addChildren([price]),
		history: createMemoryHistory({ initialEntries: [`/price${search}`] }),
	});
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<RouterProvider router={router as never} />
			</TooltipProvider>
		</QueryClientProvider>,
	);
}

describe("pricing workbench", () => {
	it("shows a Monte-Carlo price with its standard error (± …)", async () => {
		renderWorkbench();
		expect(
			await screen.findByRole("heading", { name: /european option/i }),
		).toBeInTheDocument();
		expect(await screen.findByText(/±/)).toBeInTheDocument();
	});

	it("renders the greeks table from the response", async () => {
		renderWorkbench();
		expect(await screen.findByText("delta")).toBeInTheDocument();
		expect(await screen.findByText("vega")).toBeInTheDocument();
	});
});
