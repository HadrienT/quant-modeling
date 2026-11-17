import {
	RouterProvider,
	createMemoryHistory,
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import type { DeltaBucketRow } from "@/shared/api/types";
import { decodeParams } from "@/shared/products/workbenchLink";
import { DeltaMatrix } from "./DeltaMatrix";

const ROW = {
	ttm: 0.25,
	tenor_label: "3M",
	vol_atm: 0.1834,
} as DeltaBucketRow;

function renderMatrix(spot?: number) {
	const root = createRootRoute();
	const routes = ["/", "/price"].map((path) =>
		createRoute({
			getParentRoute: () => root,
			path,
			component: () => <DeltaMatrix rows={[ROW]} spot={spot} />,
		}),
	);
	const router = createRouter({
		routeTree: root.addChildren(routes),
		history: createMemoryHistory({ initialEntries: ["/"] }),
	});
	return render(<RouterProvider router={router as never} />);
}

/** The bridge from the vol matrix to the workbench (WP 28, lot U2). */
describe("vol matrix → pricing workbench", () => {
	it("opens a European option struck at the spot, with the tenor and its ATM vol", async () => {
		renderMatrix(512.345);
		const link = await screen.findByRole("link", {
			name: "Price an at-the-money 3M option",
		});
		const url = new URL(link.getAttribute("href")!, "http://x");
		expect(url.pathname).toBe("/price");
		expect(url.searchParams.get("product")).toBe("vanilla");
		expect(decodeParams(url.searchParams.get("p") ?? undefined, {})).toEqual({
			spot: 512.35,
			strike: 512.35,
			maturity: 0.25,
			vol: 18.34,
		});
	});

	it("offers no link without a spot (workbench hidden from the visitor)", async () => {
		renderMatrix();
		await screen.findByText("3M");
		expect(screen.queryByRole("link")).toBeNull();
	});
});
