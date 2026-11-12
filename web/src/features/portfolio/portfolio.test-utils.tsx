import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { QueryClientProvider } from "@tanstack/react-query";
import { render } from "@testing-library/react";
import { z } from "zod";
import { makeQueryClient } from "@/shared/api";
import { SessionProvider } from "@/shared/session";
import { TooltipProvider } from "@/shared/ui";
import PortfolioPage from "./PortfolioPage";

/** The Portfolio page at /portfolio, with the providers the app gives it. */
export function renderPortfolioPage() {
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
