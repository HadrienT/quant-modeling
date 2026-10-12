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
});
