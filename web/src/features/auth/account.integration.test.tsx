import {
	Outlet,
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { SessionProvider, __TOKEN_KEY } from "@/shared/session";
import { TooltipProvider } from "@/shared/ui";
import ProfilePage from "./ProfilePage";
import { SessionMenu } from "./SessionMenu";

/**
 * Account UI against the MSW fixtures (a signed-in Google account): the nav
 * shows an avatar menu, and /profile names the account by its email — never
 * by the opaque `google:<sub>` username.
 */
function renderAt(path: string) {
	const root = createRootRoute({
		component: () => (
			<>
				<SessionMenu />
				<Outlet />
			</>
		),
	});
	const profile = createRoute({
		getParentRoute: () => root,
		path: "/profile",
		component: ProfilePage,
	});
	const router = createRouter({
		routeTree: root.addChildren([profile]),
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

describe("account", () => {
	beforeEach(() => localStorage.setItem(__TOKEN_KEY, "mock.jwt.token"));
	afterEach(() => localStorage.clear());

	it("a signed-in account shows an avatar button, named by the email", async () => {
		// The menu it opens is Radix's DropdownMenu, whose pointer handling
		// jsdom cannot run; its content is checked in a real browser.
		renderAt("/");
		const trigger = await screen.findByRole("button", {
			name: "Account menu for alice@example.com",
		});
		expect(trigger).toHaveAttribute("aria-haspopup", "menu");
		expect(trigger).toHaveTextContent("A");
	});

	it("the profile names a Google account by its email, not its opaque id", async () => {
		renderAt("/profile");
		expect(
			await screen.findByRole("heading", { name: "alice@example.com" }),
		).toBeInTheDocument();
		expect(screen.getByText("Google account")).toBeInTheDocument();
		expect(screen.queryByText("google:1234567890")).not.toBeInTheDocument();
	});
});
