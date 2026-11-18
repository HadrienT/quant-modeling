import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor, within } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { server } from "@/shared/test/msw-server";
import rates from "@/shared/test/rates.fixtures.json";
import { TooltipProvider } from "@/shared/ui";
import PricingPage from "../PricingPage";

/**
 * The swap and the swaptions of the workbench are scripts of the library
 * (issue 313, lot b2b), priced on the SOFR market of the traded swaps and
 * swaptions; the rates engines price the same trade in their Diagnostics,
 * against real API responses (shared/test/rates.fixtures.json).
 */

type Scripted = { product: string; hull_white?: Record<string, unknown> };
type Swaption = {
	swaption: {
		exercise: string;
		model: string;
		strike: number;
		expiry: number;
		tenor: number;
	};
};

function mount(product: string, diag = false) {
	const scripted: Scripted[] = [];
	const swaptions: Swaption[] = [];
	server.use(
		http.post("*/price/scripted-product", async ({ request }) => {
			scripted.push((await request.json()) as Scripted);
			return HttpResponse.json({
				npv: 191_794.5,
				greeks: { rho: 2.07e7, vega: 1.54e7 },
				bumped_greeks: ["rho", "vega"],
				diagnostics: "Analytic: European payer swaption under Hull-White",
				mc_std_error: 0,
				warnings: [],
			});
		}),
		http.post("*/price/rates/swaption", async ({ request }) => {
			const body = (await request.json()) as Swaption;
			swaptions.push(body);
			return HttpResponse.json(
				rates.swaption[
					body.swaption.exercise === "bermudan" ? "bermudan" : "european"
				],
			);
		}),
	);
	const root = createRootRoute();
	const price = createRoute({
		getParentRoute: () => root,
		path: "/price",
		validateSearch: (s: Record<string, unknown>) => s,
		component: PricingPage,
	});
	const router = createRouter({
		routeTree: root.addChildren([price]),
		history: createMemoryHistory({
			initialEntries: [`/price?product=${product}${diag ? "&diag=true" : ""}`],
		}),
	});
	render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<RouterProvider router={router as never} />
			</TooltipProvider>
		</QueryClientProvider>,
	);
	return { scripted, swaptions };
}

describe("the swap and the swaptions are scripts", () => {
	it("opens the swap script from the former link, on the SOFR market", async () => {
		const { scripted } = mount("swap");
		expect(
			await screen.findByRole("heading", { name: "Interest rate swap" }),
		).toBeInTheDocument();
		await waitFor(() => expect(scripted.length).toBeGreaterThan(0));
		expect(scripted[0]).toMatchObject({
			product: "interest-rate-swap",
			hull_white: { curve: "sofr" },
		});
		// no government curve to choose: the market is the traded swaps
		expect(screen.queryByText("Government curve")).not.toBeInTheDocument();
	});

	it("prices the same swap on the rates engines in its diagnostics", async () => {
		mount("swap", true);
		const same = await screen.findByRole("region", {
			name: "The same trade on the rates engines",
		});
		expect(await within(same).findByText("Par rate")).toBeInTheDocument();
		expect(await within(same).findByText(/One curve:/)).toBeInTheDocument();
	});

	it("sends the script's swaption to the rates engines under Hull-White", async () => {
		const { swaptions } = mount("swaption", true);
		expect(
			await screen.findByRole("heading", { name: "European swaption" }),
		).toBeInTheDocument();
		await waitFor(() => expect(swaptions.length).toBeGreaterThan(0));
		expect(swaptions[0]!.swaption).toMatchObject({
			exercise: "european",
			model: "hull_white",
			expiry: 1,
			tenor: 5,
			strike: 0.046,
		});
		expect(
			await screen.findByText("Hull-White calibration"),
		).toBeInTheDocument();
		expect(
			screen.getAllByText("SABR (shifted, Hagan 2002)").length,
		).toBeGreaterThan(0);
	});

	it("compares the Bermudan script with the lattice", async () => {
		const { swaptions } = mount("script-bermudan-swaption", true);
		await waitFor(() =>
			expect(swaptions.at(-1)?.swaption.exercise).toBe("bermudan"),
		);
		expect(await screen.findByText(/Longstaff-Schwartz/)).toBeInTheDocument();
		expect(await screen.findByText("lattice")).toBeInTheDocument();
	});
});
