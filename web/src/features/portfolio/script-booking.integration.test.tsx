import { fireEvent, screen, waitFor } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { encodeParams } from "@/shared/products";
import * as fx from "@/shared/test/fixtures";
import { server } from "@/shared/test/msw-server";
import { renderPortfolioPage } from "./portfolio.test-utils";
import { workbenchFromPosition } from "./workbenchFromPosition";

/**
 * A product of the script library booked in a portfolio (issue 313, lot b3):
 * the API renders its script on the trade date and values it, the trade is
 * booked on that position at that value, and the position links back to the
 * Price page with its terms.
 */
const KEY = "qm_local_portfolios";

const SPEC = {
	kind: "script" as const,
	product: "european-call",
	title: "European call",
	terms: { strike: 1.05 },
	script:
		"2026-11-23\n    strike = 1.05\n    s0 = spot()\n\n2027-11-22\n    pays max(spot() - strike * s0, 0)\n",
	underlying: "AAPL",
	currency: "USD" as const,
	start: "2026-11-23",
	expiry: "2027-11-22",
	curve: null,
	closed_form: null,
};

describe("booking a library product", () => {
	beforeEach(() =>
		localStorage.setItem(KEY, JSON.stringify([fx.portfolio("Long book")])),
	);
	afterEach(() => localStorage.clear());

	it("books the script the API rendered, at its value that day", async () => {
		const asked: Record<string, unknown>[] = [];
		server.use(
			http.post(
				"*/api/portfolio-valuation/script/quote",
				async ({ request }) => {
					asked.push((await request.json()) as Record<string, unknown>);
					return HttpResponse.json({
						spec: SPEC,
						label: "AAPL European call 2027-11-22",
						value: 12.5,
						currency: "USD",
					});
				},
			),
		);
		const trade = encodeParams({
			product: "script-european-call",
			values: { strike: 105, tickers: "AAPL" },
		});
		renderPortfolioPage(`/portfolio?trade=${trade}`);
		expect(await screen.findByText(/worth 12\.5000 USD/)).toBeInTheDocument();
		expect(asked.at(-1)).toMatchObject({
			product: "european-call",
			terms: { strike: 1.05 },
			underlying: "AAPL",
		});
		fireEvent.click(screen.getByRole("button", { name: "Book purchase" }));
		await waitFor(() => {
			const [pf] = JSON.parse(localStorage.getItem(KEY)!);
			const inst = pf.instruments.find(
				(i: { spec: { kind: string } }) => i.spec.kind === "script",
			);
			expect(inst.spec.product).toBe("european-call");
			const t = pf.transactions.find(
				(x: { instrument_id: string }) => x.instrument_id === inst.id,
			);
			expect(t.price).toBe(12.5);
			expect(t.quantity).toBe(1);
		});
	});

	it("links a position back to the Price page with its terms", () => {
		const link = workbenchFromPosition(
			{ id: "x", label: "call", spec: SPEC, netting_set_id: null },
			"2026-12-01",
		);
		expect(link?.product).toBe("script-european-call");
		// a converted position has no library product to open
		const custom = { ...SPEC, product: "custom" };
		expect(
			workbenchFromPosition({
				id: "y",
				label: "c",
				spec: custom,
				netting_set_id: null,
			}),
		).toBeNull();
	});
});
