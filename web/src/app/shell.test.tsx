import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { AppLayout } from "./layout/AppLayout";
import { RouteError } from "./layout/RouteError";

function renderShell(initialPath = "/ok") {
	const root = createRootRoute({
		component: AppLayout,
		errorComponent: RouteError,
	});
	const ok = createRoute({
		getParentRoute: () => root,
		path: "/ok",
		component: () => <p>content ok</p>,
	});
	const boom = createRoute({
		getParentRoute: () => root,
		path: "/boom",
		component: () => {
			throw new Error("simulated page failure");
		},
	});
	const router = createRouter({
		routeTree: root.addChildren([ok, boom]),
		history: createMemoryHistory({ initialEntries: [initialPath] }),
	});
	const qc = makeQueryClient();
	return render(
		<QueryClientProvider client={qc}>
			<RouterProvider router={router as never} />
		</QueryClientProvider>,
	);
}

describe("app shell", () => {
	it("renders nav, page content and the API health footer", async () => {
		renderShell();
		expect(
			await screen.findByRole("link", { name: /quant modeling/i }),
		).toBeInTheDocument();
		expect(await screen.findByText("content ok")).toBeInTheDocument();
		expect(await screen.findByText(/API online/i)).toBeInTheDocument();
	});

	it("shows the route error boundary instead of a blank screen", async () => {
		renderShell("/boom");
		expect(await screen.findByRole("alert")).toHaveTextContent(
			/simulated page failure/i,
		);
	});

	it("toggles the theme attribute on <html>", async () => {
		renderShell();
		const user = userEvent.setup();
		const before =
			document.documentElement.getAttribute("data-theme") ?? "dark";
		await user.click(
			await screen.findByRole("button", { name: /switch to .* theme/i }),
		);
		expect(document.documentElement.getAttribute("data-theme")).not.toBe(
			before,
		);
	});
});
