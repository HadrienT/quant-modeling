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
import { afterEach, describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { SessionProvider, __TOKEN_KEY, useNavPages } from "@/shared/session";
import { server } from "@/shared/test/msw-server";
import { TooltipProvider } from "@/shared/ui";
import { AppLayout } from "./layout/AppLayout";

/**
 * Pages an administrator hid (admin panel): a visitor loses them from the
 * navigation and lands on "not available"; an administrator keeps them, under
 * a banner.
 */
/** A page that, like Pricing, reads the visibility itself to hide its links. */
function PageReadingVisibility({ path }: { path: string }) {
	useNavPages();
	return <p>content of {path}</p>;
}

function renderAt(path: string) {
	const root = createRootRoute({ component: AppLayout });
	const pages = ["/xva", "/credit", "/price"].map((p) =>
		createRoute({
			getParentRoute: () => root,
			path: p,
			component: () => <PageReadingVisibility path={p} />,
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
		// The pages sit in the menu of their space (WP 28).
		fireEvent.click(screen.getByRole("button", { name: "Market" }));
		expect(screen.getByRole("link", { name: /^Credit/ })).toBeInTheDocument();
		// close the first menu before opening the next, as a pointer would
		fireEvent.click(screen.getByRole("button", { name: "Market" }));
		fireEvent.click(screen.getByRole("button", { name: "Risk" }));
		expect(
			screen.getByRole("link", { name: /^Portfolio/ }),
		).toBeInTheDocument();
		expect(screen.queryByRole("link", { name: /xVA/ })).toBeNull();
	});

	it("drop out of the tab strip of their space (WP 28, U5)", async () => {
		hide("/backtest");
		renderAt("/xva");
		const strip = await screen.findByRole("navigation", {
			name: "Risk pages",
		});
		await waitFor(() =>
			expect(
				within(strip).queryByRole("link", { name: "Backtest" }),
			).toBeNull(),
		);
		expect(within(strip).getByRole("link", { name: "xVA" })).toHaveAttribute(
			"aria-current",
			"page",
		);
		expect(
			within(strip).getByRole("link", { name: "Portfolio" }),
		).toBeInTheDocument();
	});

	it("leave no tab strip to a space reduced to one page", async () => {
		hide("/scripting", "/visualize");
		renderAt("/price");
		expect(await screen.findByText("content of /price")).toBeInTheDocument();
		expect(
			screen.queryByRole("navigation", { name: "Price pages" }),
		).toBeNull();
	});

	it("keep About a link of its own in the bar, unless hidden", async () => {
		const { unmount } = renderAt("/credit");
		await screen.findByText("content of /credit");
		// In sight without opening any menu.
		expect(
			await screen.findByRole("link", { name: "About" }),
		).toBeInTheDocument();
		unmount();
		hide("/about");
		renderAt("/credit");
		await screen.findByText("content of /credit");
		await waitFor(() =>
			expect(screen.queryByRole("link", { name: "About" })).toBeNull(),
		);
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
		fireEvent.click(screen.getByRole("button", { name: "Risk" }));
		expect(
			screen.getByRole("link", { name: /hidden from visitors: xVA/ }),
		).toBeInTheDocument();
	});

	it("render once an expired session is answered, without asking again", async () => {
		// An expired token: /me answers 401. The page used to flip back to its
		// fallback on every refetch, remount, refetch again: a white page
		// asking /me hundreds of times a second.
		localStorage.setItem(__TOKEN_KEY, "expired.jwt.token");
		let calls = 0;
		server.use(
			http.get("*/api/auth/me", () => {
				calls += 1;
				return HttpResponse.json({ detail: "expired" }, { status: 401 });
			}),
		);
		renderAt("/price");
		expect(await screen.findByText("content of /price")).toBeInTheDocument();
		await new Promise((r) => setTimeout(r, 300));
		expect(screen.getByText("content of /price")).toBeInTheDocument();
		expect(calls).toBeLessThanOrEqual(2);
		// The dead token is dropped: the visitor is simply signed out.
		expect(localStorage.getItem(__TOKEN_KEY)).toBeNull();
	});
});
