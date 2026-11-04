import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { afterEach, describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { SessionProvider, __TOKEN_KEY } from "@/shared/session";
import { server } from "@/shared/test/msw-server";
import { TooltipProvider } from "@/shared/ui";
import { AppLayout } from "./layout/AppLayout";

/**
 * Pages an administrator hid (admin panel): a visitor loses them from the
 * navigation and lands on "not available"; an administrator keeps them, under
 * a banner.
 */
function renderAt(path: string) {
	const root = createRootRoute({ component: AppLayout });
	const pages = ["/xva", "/credit"].map((p) =>
		createRoute({
			getParentRoute: () => root,
			path: p,
			component: () => <p>content of {p}</p>,
		}),
	);
	const router = createRouter({
		routeTree: root.addChildren(pages),
		history: createMemoryHistory({ initialEntries: [path] }),
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

function hide(...paths: string[]) {
	server.use(
		http.get("*/api/site/pages", () => HttpResponse.json({ hidden: paths })),
	);
}

describe("pages hidden by an administrator", () => {
	afterEach(() => localStorage.clear());

	it("are gone from a visitor's navigation, and their URL says not available", async () => {
		hide("/xva");
		renderAt("/xva");
		expect(await screen.findByText("Page not available")).toBeInTheDocument();
		expect(screen.queryByText("content of /xva")).toBeNull();
		expect(screen.getByRole("link", { name: "Credit" })).toBeInTheDocument();
		expect(screen.queryByRole("link", { name: /xVA/ })).toBeNull();
	});

	it("leave the other pages alone", async () => {
		hide("/xva");
		renderAt("/credit");
		expect(await screen.findByText("content of /credit")).toBeInTheDocument();
	});

	it("stay visible to an administrator, flagged as hidden", async () => {
		hide("/xva");
		localStorage.setItem(__TOKEN_KEY, "mock.jwt.token");
		server.use(
			http.get("*/api/auth/me", () =>
				HttpResponse.json({ username: "boss", is_admin: true }),
			),
		);
		renderAt("/xva");
		expect(await screen.findByText("content of /xva")).toBeInTheDocument();
		expect(
			await screen.findByText(/only administrators see this page/),
		).toBeInTheDocument();
		expect(
			screen.getByRole("link", { name: /hidden from visitors: xVA/ }),
		).toBeInTheDocument();
	});
});
