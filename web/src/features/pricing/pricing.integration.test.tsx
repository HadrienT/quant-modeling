import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { fireEvent, render, screen } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { TooltipProvider } from "@/shared/ui";
import { pricingResponse } from "@/shared/test/fixtures";
import { server } from "@/shared/test/msw-server";
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

	it("says how long the pricing took on the server", async () => {
		renderWorkbench();
		expect(await screen.findByText(/computed in 3\.42 ms/)).toBeInTheDocument();
	});

	it("greys the GPU out, with no race, on a CPU-only server", async () => {
		renderWorkbench();
		expect(
			await screen.findByText(/no GPU on this server/),
		).toBeInTheDocument();
		expect(screen.getByRole("button", { name: /GPU/ })).toBeDisabled();
		expect(screen.queryByText("CPU vs GPU")).not.toBeInTheDocument();
	});

	it("races the same pricing on CPU and GPU and reports the speed-up", async () => {
		server.use(
			http.get("*/price/devices", () =>
				HttpResponse.json({ cpu: "Xeon", gpus: ["V100"], gpu_compiled: true }),
			),
			http.post("*/price/option/vanilla", async ({ request }) => {
				const body = (await request.json()) as { device?: string };
				const gpu = body.device === "gpu";
				return HttpResponse.json(
					pricingResponse({
						device: gpu ? "gpu" : "cpu",
						compute_ms: gpu ? 5 : 1500,
					}),
				);
			}),
		);
		renderWorkbench("?product=vanilla&engine=mc&p=eyJuX3BhdGhzIjoyMDAwMDAwMH0");
		fireEvent.click(await screen.findByRole("button", { name: /race/i }));
		expect(await screen.findByText("300×")).toBeInTheDocument();
		expect(screen.getByText(/price gap/)).toBeInTheDocument();
	});

	it("renders the greeks table from the response", async () => {
		renderWorkbench();
		expect(await screen.findByText("delta")).toBeInTheDocument();
		expect(await screen.findByText("vega")).toBeInTheDocument();
	});
});
