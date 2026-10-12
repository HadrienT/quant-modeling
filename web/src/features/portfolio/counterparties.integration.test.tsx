import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { z } from "zod";
import { makeQueryClient } from "@/shared/api";
import type { Portfolio } from "@/shared/api";
import { SessionProvider } from "@/shared/session";
import * as fx from "@/shared/test/fixtures";
import { TooltipProvider } from "@/shared/ui";
import PortfolioPage from "./PortfolioPage";

/**
 * The Counterparties tab of a portfolio kept in this browser: the examples
 * are copied in, a counterparty is created with its netting set, a
 * derivative is traded under one — and a counterparty still faced stays.
 */
const KEY = "qm_local_portfolios";
const stored = () =>
	(JSON.parse(localStorage.getItem(KEY) ?? "[]") as Portfolio[])[0]!;

function renderPage() {
	const root = createRootRoute();
	const page = createRoute({
		getParentRoute: () => root,
		path: "/portfolio",
		validateSearch: z.object({ id: z.string().optional() }).parse,
		component: PortfolioPage,
	});
	const router = createRouter({
		routeTree: root.addChildren([page]),
		history: createMemoryHistory({ initialEntries: ["/portfolio"] }),
	});
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<SessionProvider>
				<TooltipProvider>
					<RouterProvider router={router as never} />
				</TooltipProvider>
			</SessionProvider>
		</QueryClientProvider>,
	);
}

async function openTab() {
	const user = userEvent.setup();
	renderPage();
	await user.click(await screen.findByRole("tab", { name: /Counterparties/ }));
	return user;
}

describe("counterparties of a portfolio", () => {
	beforeEach(() => {
		localStorage.setItem(KEY, JSON.stringify([fx.portfolio("OTC book")]));
	});
	afterEach(() => localStorage.clear());

	it("copies the examples in, once", async () => {
		const user = await openTab();
		expect(screen.getByText("No counterparty yet.")).toBeInTheDocument();
		const add = screen.getByRole("button", {
			name: "Add example counterparties",
		});
		await waitFor(() => expect(add).toBeEnabled());
		await user.click(add);

		const dealer = await screen.findByRole("article", {
			name: "Northgate Bank (dealer)",
		});
		expect(within(dealer).getByText(/minimum transfer 500,000/)).toBeVisible();
		expect(
			within(
				screen.getByRole("article", {
					name: "Meridian Industries (corporate)",
				}),
			).getByText("Uncollateralised"),
		).toBeVisible();
		expect(stored().counterparties).toHaveLength(2);
		expect(add).toBeDisabled();
	});

	it("creates a counterparty with a collateralised netting set", async () => {
		const user = await openTab();
		await user.click(screen.getByRole("button", { name: "New counterparty" }));
		const save = screen.getByRole("button", { name: "Save counterparty" });
		expect(save).toBeDisabled(); // no name yet
		await user.type(screen.getByLabelText("Name"), "Acme Corp");
		await user.selectOptions(screen.getByLabelText("Rating"), "BB");
		await user.click(screen.getByLabelText("Collateralised (CSA)"));
		await user.click(save);

		const [c] = stored().counterparties!;
		expect(c).toMatchObject({ name: "Acme Corp", rating: "BB", recovery: 0.4 });
		expect(c!.netting_sets![0]!.csa).toMatchObject({
			minimum_transfer_amount: 500_000,
			margin_period_of_risk_days: 10,
		});
	});

	it("trades a derivative under a netting set, which then keeps its counterparty", async () => {
		const user = await openTab();
		const add = screen.getByRole("button", {
			name: "Add example counterparties",
		});
		await waitFor(() => expect(add).toBeEnabled());
		await user.click(add);
		await screen.findByRole("article", { name: "Northgate Bank (dealer)" });

		await user.selectOptions(
			screen.getByLabelText(/^Netting set of/),
			"example-dealer-vm",
		);
		await waitFor(() =>
			expect(
				stored().instruments!.find((i) => i.id === "cac-call")!.netting_set_id,
			).toBe("example-dealer-vm"),
		);
		const dealer = screen.getByRole("article", {
			name: "Northgate Bank (dealer)",
		});
		expect(within(dealer).getByText(/^1 trade:/)).toBeVisible();
		expect(
			within(dealer).getByRole("button", { name: /Delete/ }),
		).toBeDisabled();
		expect(
			within(
				screen.getByRole("article", {
					name: "Meridian Industries (corporate)",
				}),
			).getByRole("button", { name: /Delete/ }),
		).toBeEnabled();
	});

	it("books a swap as one contract, at what it is worth, under a netting set", async () => {
		const seeded = {
			...fx.portfolio("OTC book"),
			counterparties: fx.counterpartyExamples(),
		};
		localStorage.setItem(KEY, JSON.stringify([seeded]));
		const user = userEvent.setup();
		renderPage();
		await user.click(await screen.findByRole("button", { name: /New trade/ }));
		await user.selectOptions(
			screen.getByLabelText("Instrument type"),
			"rate_fx",
		);
		expect(
			await screen.findByText(/Payer swap USD 10M at 3.650%/),
		).toBeVisible();
		await user.selectOptions(
			screen.getByLabelText("Counterparty and netting set"),
			"example-corporate-isda",
		);
		await user.click(screen.getByRole("button", { name: "Book purchase" }));

		await waitFor(() => expect(stored().transactions).toHaveLength(4));
		const swap = stored().instruments!.find((i) => i.spec.kind === "rate_fx")!;
		expect(swap.netting_set_id).toBe("example-corporate-isda");
		expect(swap.spec).toMatchObject({ product: "swap", fixed_rate: 0.0365 });
		expect(stored().transactions!.at(-1)).toMatchObject({
			instrument_id: swap.id,
			quantity: 1,
			price: 0,
		});
	});

	it("computes the xVA of a netting set on demand, with what was left out", async () => {
		const pf = fx.portfolio("OTC book");
		const instruments = pf.instruments.map((i) =>
			i.id === "cac-call" ? { ...i, netting_set_id: "example-dealer-vm" } : i,
		);
		localStorage.setItem(
			KEY,
			JSON.stringify([
				{ ...pf, instruments, counterparties: fx.counterpartyExamples() },
			]),
		);
		const user = userEvent.setup();
		renderPage();
		await user.click(await screen.findByRole("tab", { name: "xVA" }));
		expect(screen.getByLabelText("Netting set")).toHaveTextContent(
			"Northgate Bank (dealer) (A): ISDA with CSA: variation margin — 1 trade",
		);
		// Nothing is simulated before it is asked for.
		expect(screen.queryByRole("tab", { name: "Exposure" })).toBeNull();
		await user.click(screen.getByRole("button", { name: "Compute xVA" }));
		expect(
			await screen.findByRole("tab", { name: "Exposure" }),
		).toBeInTheDocument();
		expect(screen.getByText(/is left out of this netting set/)).toBeVisible();
		// The share its options are on, and what its model rests on.
		expect(
			screen.getByRole("heading", { name: "The share: ^FCHI" }),
		).toBeVisible();
		expect(
			screen.getByText(/\+0\.12 \(95 % interval −0\.04 to \+0\.27\)/),
		).toBeVisible();
		expect(
			screen.getByRole("heading", {
				name: "Northgate Bank (dealer): ISDA with CSA: variation margin",
			}),
		).toBeVisible();
		// The sensitivities are another simulation, asked for from their tab.
		await user.click(screen.getByRole("tab", { name: "Sensitivities" }));
		await user.click(
			screen.getByRole("button", { name: "Compute the sensitivities" }),
		);
		expect(await screen.findByText("Par swap rates")).toBeVisible();
	});

	it("says what to do when no trade is under a netting set", async () => {
		const user = userEvent.setup();
		renderPage();
		await user.click(await screen.findByRole("tab", { name: "xVA" }));
		expect(
			screen.getByText(/No open trade is under a netting set yet/),
		).toBeVisible();
	});
});
