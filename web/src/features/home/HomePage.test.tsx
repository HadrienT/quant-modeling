import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { afterEach, describe, expect, it, vi } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { SessionProvider, __TOKEN_KEY } from "@/shared/session";
import { server } from "@/shared/test/msw-server";
import HomePage from "./HomePage";

// three.js has no WebGL in jsdom, and the surface is not what is tested here.
vi.mock("./HeroSurface", () => ({ default: () => null }));

/**
 * The home page links to the pages like the navigation does, so it follows
 * the admin panel too: a page hidden from visitors is not offered on it.
 */
function renderHome() {
	const root = createRootRoute();
	const home = createRoute({
		getParentRoute: () => root,
		path: "/",
		component: HomePage,
	});
	const router = createRouter({
		routeTree: root.addChildren([home]),
		history: createMemoryHistory({ initialEntries: ["/"] }),
	});
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<SessionProvider>
				<RouterProvider router={router as never} />
			</SessionProvider>
		</QueryClientProvider>,
	);
}

function hide(...paths: string[]) {
	server.use(
		http.get("*/api/site/pages", () => HttpResponse.json({ hidden: paths })),
	);
}

describe("home page links to hidden pages", () => {
	afterEach(() => localStorage.clear());

	it("are not offered to a visitor", async () => {
		hide("/xva", "/price", "/about");
		renderHome();
		await screen.findByRole("link", { name: /Backtest/ });
		await waitFor(() =>
			expect(screen.queryByRole("link", { name: /xVA/ })).toBeNull(),
		);
		expect(screen.queryByRole("link", { name: /Price an option/ })).toBeNull();
		expect(screen.queryByRole("link", { name: /how it is built/ })).toBeNull();
		expect(screen.getByRole("link", { name: /Script a payoff/ })).toBeVisible();
		expect(screen.getByRole("link", { name: /Backtest/ })).toBeVisible();
	});

	it("drop the tile section when every page is hidden", async () => {
		hide(
			"/price",
			"/scripting",
			"/market",
			"/xva",
			"/simulation",
			"/portfolio",
			"/backtest",
			"/visualize",
			"/products",
		);
		renderHome();
		await screen.findByText("Depth over breadth");
		await waitFor(() =>
			expect(screen.queryByText("What you can do")).toBeNull(),
		);
		expect(screen.queryByRole("link", { name: /Price an option/ })).toBeNull();
	});

	it("stay for an administrator, flagged as hidden", async () => {
		hide("/xva");
		localStorage.setItem(__TOKEN_KEY, "mock.jwt.token");
		server.use(
			http.get("*/api/auth/me", () =>
				HttpResponse.json({ username: "boss", is_admin: true }),
			),
		);
		renderHome();
		const tile = await screen.findByRole("link", {
			name: /xVA.*Hidden from visitors/,
		});
		expect(tile).toBeVisible();
	});
});
