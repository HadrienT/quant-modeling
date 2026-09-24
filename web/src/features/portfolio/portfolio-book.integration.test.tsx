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
 * Managing portfolios kept in this browser (anonymous visitor): the sidebar
 * lists them; one is renamed in place, one deleted after confirmation; a
 * position booked by mistake is deleted with all its trades.
 */
const KEY = "qm_local_portfolios";
const stored = () =>
	JSON.parse(localStorage.getItem(KEY) ?? "[]") as Portfolio[];

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

describe("portfolio book", () => {
	beforeEach(() => {
		const a = {
			...fx.portfolio("Long book"),
			created_at: "2026-01-01T00:00:00Z",
		};
		const b = { ...fx.portfolio("Hedges"), created_at: "2026-02-01T00:00:00Z" };
		localStorage.setItem(KEY, JSON.stringify([a, b]));
	});
	afterEach(() => localStorage.clear());

	it("lists every portfolio in the sidebar, the first one open", async () => {
		renderPage();
		const nav = await screen.findByRole("complementary", {
			name: "Portfolios",
		});
		const items = within(nav).getAllByRole("button", { name: /positions/ });
		expect(items.map((b) => b.textContent)).toEqual([
			expect.stringContaining("Long book"),
			expect.stringContaining("Hedges"),
		]);
		expect(items[0]).toHaveAttribute("aria-current", "page");
		expect(
			await screen.findByRole("heading", { name: "Long book" }),
		).toBeInTheDocument();
	});

	it("renames a portfolio in place", async () => {
		renderPage();
		await userEvent.click(
			await screen.findByRole("button", { name: "Rename Hedges" }),
		);
		const field = screen.getByRole("textbox", { name: "Portfolio name" });
		await userEvent.clear(field);
		await userEvent.type(field, "Tail hedges{Enter}");
		expect(
			await screen.findByRole("button", { name: /^Tail hedges/ }),
		).toBeInTheDocument();
		expect(stored().map((p) => p.name)).toEqual(["Long book", "Tail hedges"]);
	});

	it("deletes a portfolio only after confirming, and opens the next one", async () => {
		renderPage();
		await userEvent.click(
			await screen.findByRole("button", { name: "Delete Long book" }),
		);
		const dialog = await screen.findByRole("dialog");
		expect(within(dialog).getByText(/cannot be undone/)).toBeInTheDocument();
		await userEvent.click(
			within(dialog).getByRole("button", { name: "Delete portfolio" }),
		);
		await waitFor(() =>
			expect(stored().map((p) => p.name)).toEqual(["Hedges"]),
		);
		expect(
			await screen.findByRole("heading", { name: "Hedges" }),
		).toBeInTheDocument();
	});

	it("deletes a position booked by mistake with all its trades", async () => {
		renderPage();
		const row = await screen.findByRole("row", { name: /MC\.PA — LVMH/ });
		await userEvent.click(
			within(row).getByRole("button", { name: /Delete position/ }),
		);
		const dialog = await screen.findByRole("dialog");
		expect(
			within(dialog).getByText(/Its 2 trades leave the journal/),
		).toBeInTheDocument();
		await userEvent.click(
			within(dialog).getByRole("button", { name: "Delete position" }),
		);
		await waitFor(() => {
			const [first] = stored();
			expect(first!.transactions!.map((t) => t.instrument_id)).toEqual([
				"cac-call",
			]);
			expect(first!.instruments!.map((i) => i.id)).toEqual(["cac-call"]);
		});
	});

	it("opens a demo read-only, and copies it into one's own portfolios", async () => {
		renderPage();
		await userEvent.click(await screen.findByRole("tab", { name: "Demos" }));
		await userEvent.click(
			await screen.findByRole("button", { name: /Euro blue chips/ }),
		);
		expect(
			await screen.findByText(/Demo portfolio · read-only/),
		).toBeInTheDocument();
		expect(await screen.findByText("Market value")).toBeInTheDocument();
		// nothing that edits the ledger
		expect(
			screen.queryByRole("button", { name: /New trade/ }),
		).not.toBeInTheDocument();
		const row = await screen.findByRole("row", { name: /MC\.PA — LVMH/ });
		expect(
			within(row).queryByRole("button", { name: "Sell" }),
		).not.toBeInTheDocument();

		await userEvent.click(
			screen.getByRole("button", { name: /Copy to my portfolios/ }),
		);
		await waitFor(() =>
			expect(stored().map((p) => p.name)).toEqual([
				"Long book",
				"Hedges",
				"Euro blue chips",
			]),
		);
		const copy = stored()[2]!;
		expect(copy.id).not.toBe("demo-euro-blue-chips");
		expect(copy.owner).toBe("");
		expect(copy.transactions).toHaveLength(3);
		// the copy is now open, and editable
		expect(
			await screen.findByRole("button", { name: /New trade/ }),
		).toBeInTheDocument();
	});
});
