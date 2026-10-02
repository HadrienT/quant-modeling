import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import {
	fireEvent,
	render,
	screen,
	waitFor,
	within,
} from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { server } from "@/shared/test/msw-server";
import rates from "@/shared/test/rates.fixtures.json";
import { TooltipProvider } from "@/shared/ui";
import PricingPage from "../PricingPage";

/**
 * The rates products of the pricing workbench against real API responses
 * (shared/test/rates.fixtures.json): a swap and a swaption are priced on a
 * quote set that says whether it is market data, the swaption's model is
 * chosen and announced, an edited quote is sent only when asked, and a set
 * the store cannot back is an error, never a silent switch.
 */

type Body = {
	curves: { fras: unknown[]; ois: { rate: number }[] };
	swaption?: { exercise?: string; model?: string };
};

function mount(product: "swap" | "swaption") {
	const bodies: Body[] = [];
	server.use(
		http.post("*/price/rates/swap", async ({ request }) => {
			bodies.push((await request.json()) as Body);
			return HttpResponse.json(rates.swap);
		}),
		http.post("*/price/rates/swaption", async ({ request }) => {
			const body = (await request.json()) as Body;
			bodies.push(body);
			return HttpResponse.json(
				rates.swaption[
					body.swaption?.exercise === "bermudan" ? "bermudan" : "european"
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
			initialEntries: [`/price?product=${product}`],
		}),
	});
	render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<RouterProvider router={router as never} />
			</TooltipProvider>
		</QueryClientProvider>,
	);
	return bodies;
}

const pick = (label: string) =>
	fireEvent.click(screen.getByRole("button", { name: label }));

describe("interest rate swap in the workbench", () => {
	it("prices on the traded USD curve by default and says it is one curve", async () => {
		const bodies = mount("swap");
		expect(
			await screen.findByRole("heading", { name: "Interest rate swap" }),
		).toBeInTheDocument();
		expect(await screen.findByText("Traded prices")).toBeInTheDocument();
		expect(await screen.findByText("Par rate")).toBeInTheDocument();
		expect(screen.getByText("market")).toBeInTheDocument();
		// SOFR discounts and projects: no index quotes, no basis chart.
		expect(bodies[0]!.curves.fras).toHaveLength(0);
		expect(await screen.findByText(/One curve:/)).toBeInTheDocument();
		expect(
			screen.queryByText("Index − OIS forward basis"),
		).not.toBeInTheDocument();
	});

	it("shows two curves and their basis on the illustrative EUR set", async () => {
		const bodies = mount("swap");
		await screen.findByText("Par rate");
		pick("EUR · illustrative, two curves");
		expect(await screen.findByText("Manual input")).toBeInTheDocument();
		expect(screen.getByText(/not market data/)).toBeInTheDocument();
		expect(
			await screen.findByText("Index − OIS forward basis"),
		).toBeInTheDocument();
		await waitFor(() => expect(bodies.at(-1)!.curves.fras).toHaveLength(2));
		expect(screen.getByText("manual")).toBeInTheDocument();
	});

	it("sends an edited quote only when asked to reprice, and calls it manual", async () => {
		const bodies = mount("swap");
		await screen.findByText("Par rate");
		const sent = bodies.length;
		fireEvent.click(screen.getByText("Edit the quotes"));
		const reprice = screen.getByRole("button", {
			name: "Reprice on these quotes",
		});
		expect(reprice).toBeDisabled();
		fireEvent.change(screen.getByLabelText("Par OIS swaps row 1 Rate"), {
			target: { value: "2.5" },
		});
		expect(bodies).toHaveLength(sent);
		fireEvent.click(reprice);
		await waitFor(() => expect(bodies.length).toBeGreaterThan(sent));
		expect(bodies.at(-1)!.curves.ois[0]!.rate).toBeCloseTo(0.025, 12);
		// Edited quotes are no longer the market's.
		expect(await screen.findByText("manual")).toBeInTheDocument();
		expect(screen.getByText(/· edited/)).toBeInTheDocument();
	});

	it("says what the store is missing instead of switching quote set", async () => {
		server.use(
			http.get("*/api/rates/quotes/usd-sofr", () =>
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
		mount("swap");
		expect(await screen.findByRole("alert")).toHaveTextContent("30 days ago");
		expect(screen.queryByText("Par rate")).not.toBeInTheDocument();
		// The other set is one click away, and it is the user's click.
		pick("EUR · illustrative, two curves");
		expect(await screen.findByText("Par rate")).toBeInTheDocument();
	});
});

describe("swaption in the workbench", () => {
	it("announces the model it chose and compares every model", async () => {
		const bodies = mount("swaption");
		const decision = await screen.findByRole("region", {
			name: "Pricing model",
		});
		expect(
			within(decision).getByText("Bachelier (normal)"),
		).toBeInTheDocument();
		expect(
			within(decision).getByText("chosen automatically"),
		).toBeInTheDocument();
		expect(within(decision).getByText(/quote convention/)).toBeInTheDocument();
		expect(bodies[0]!.swaption).toMatchObject({
			exercise: "european",
			model: "auto",
		});
		for (const model of [
			"Hull-White (calibrated)",
			"SABR (shifted, Hagan 2002)",
		])
			expect(screen.getAllByText(model).length).toBeGreaterThan(0);
		expect(screen.getByText("Hull-White calibration")).toBeInTheDocument();
		expect(screen.getByText("Switch premium")).toBeInTheDocument();
	});

	it("prices a Bermudan under Hull-White and keeps the single-rate models out", async () => {
		const bodies = mount("swaption");
		await screen.findByRole("region", { name: "Pricing model" });
		pick("Bermudan");
		await waitFor(() =>
			expect(bodies.at(-1)!.swaption!.exercise).toBe("bermudan"),
		);
		const decision = await screen.findByRole("region", {
			name: "Pricing model",
		});
		await waitFor(() =>
			expect(within(decision).getByText(/several dates/)).toBeInTheDocument(),
		);
		expect(screen.getByText("lattice")).toBeInTheDocument();
		expect(
			screen.getByRole("option", { name: "SABR (shifted)" }),
		).toBeDisabled();
		expect(
			screen.getByRole("option", { name: "Hull-White (calibrated)" }),
		).toBeEnabled();
	});
});
