import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { z } from "zod";
import { makeQueryClient } from "@/shared/api";
import { SessionProvider, __TOKEN_KEY } from "@/shared/session";
import { TooltipProvider } from "@/shared/ui";
import PortfolioPage from "./PortfolioPage";

/**
 * The portfolio page against the MSW fixtures (a signed-in account, one
 * ledger): today's MTM and P&L, positions with their input statuses, the
 * journal with signed cash flows, and the methodology.
 */
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

describe("portfolio page", () => {
	beforeEach(() => localStorage.setItem(__TOKEN_KEY, "mock.jwt.token"));
	afterEach(() => localStorage.clear());

	it("shows the portfolio marked to market, with day and total P&L", async () => {
		renderPage();
		expect(await screen.findByText("Market value")).toBeInTheDocument();
		expect(screen.getByText("Day P&L")).toBeInTheDocument();
		expect(screen.getByText("+127.60")).toBeInTheDocument();
		expect(screen.getByText("−3,690.20")).toBeInTheDocument();
		expect(
			screen.getByRole("heading", { name: "Methodology" }),
		).toBeInTheDocument();
	});

	it("lists positions with the status of their weakest input", async () => {
		renderPage();
		const row = await screen.findByRole("row", {
			name: /\^FCHI European option/,
		});
		const option = within(row).getByRole("button", { name: /\^FCHI/ });
		expect(within(row).getByText("proxied")).toBeInTheDocument();
		await userEvent.click(option);
		expect(
			await screen.findByText("vol (63-day realised)"),
		).toBeInTheDocument();
	});

	it("keeps a journal where a sale is shown as cash received", async () => {
		renderPage();
		await userEvent.click(
			await screen.findByRole("tab", { name: /Transactions/ }),
		);
		const trim = await screen.findByRole("row", { name: /trim/ });
		expect(within(trim).getByText("sell")).toBeInTheDocument();
		// 30 × 455 − 5 fees received: positive in the cash-flow convention
		expect(within(trim).getByText("+13,645.00")).toBeInTheDocument();
	});

	it("prefills a sale that closes the position from its row", async () => {
		renderPage();
		const lvmh = await screen.findByRole("row", { name: /MC\.PA — LVMH/ });
		await userEvent.click(within(lvmh).getByRole("button", { name: "Sell" }));
		expect(
			screen.getByRole("button", { name: "Book sale" }),
		).toBeInTheDocument();
		expect(screen.getByLabelText("Quantity")).toHaveValue(70);
	});
});
