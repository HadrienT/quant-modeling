import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { HttpResponse, http } from "msw";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { SessionProvider, __TOKEN_KEY } from "@/shared/session";
import { server } from "@/shared/test/msw-server";
import { TooltipProvider } from "@/shared/ui";
import AdminPage from "./AdminPage";

function renderAdmin() {
	const root = createRootRoute();
	const admin = createRoute({
		getParentRoute: () => root,
		path: "/admin",
		component: AdminPage,
	});
	const router = createRouter({
		routeTree: root.addChildren([admin]),
		history: createMemoryHistory({ initialEntries: ["/admin"] }),
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

function signInAs(isAdmin: boolean) {
	localStorage.setItem(__TOKEN_KEY, "mock.jwt.token");
	server.use(
		http.get("*/api/auth/me", () =>
			HttpResponse.json({ username: "boss", is_admin: isAdmin }),
		),
	);
}

describe("admin panel", () => {
	let saved: string[] | null = null;
	beforeEach(() => {
		saved = null;
		server.use(
			http.get("*/api/site/pages", () =>
				HttpResponse.json({ hidden: ["/backtest"] }),
			),
			http.get("*/api/admin/pages", () =>
				HttpResponse.json({
					hidden: ["/backtest"],
					updated_at: "2026-11-01T09:00:00+00:00",
					updated_by: "boss",
				}),
			),
			http.put("*/api/admin/pages", async ({ request }) => {
				saved = ((await request.json()) as { hidden: string[] }).hidden;
				return HttpResponse.json({
					hidden: saved,
					updated_at: "2026-11-04T09:00:00+00:00",
					updated_by: "boss",
				});
			}),
		);
	});
	afterEach(() => localStorage.clear());

	it("is closed to an account that is not an administrator", async () => {
		signInAs(false);
		renderAdmin();
		expect(await screen.findByText("Administrators only")).toBeInTheDocument();
		expect(screen.queryByRole("switch")).toBeNull();
	});

	it("shows which pages are hidden and hides another in one click", async () => {
		signInAs(true);
		const user = userEvent.setup();
		renderAdmin();
		const backtest = await screen.findByRole("switch", {
			name: "Show Backtest to visitors",
		});
		expect(backtest).toHaveAttribute("aria-checked", "false");
		const xva = screen.getByRole("switch", { name: "Show xVA to visitors" });
		await waitFor(() => expect(xva).toBeEnabled());
		expect(xva).toHaveAttribute("aria-checked", "true");

		await user.click(xva);
		await waitFor(() => expect(saved).toEqual(["/backtest", "/xva"]));
		await waitFor(() => expect(xva).toHaveAttribute("aria-checked", "false"));
	});
});
