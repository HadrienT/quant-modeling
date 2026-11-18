import { fireEvent, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { encodeParams } from "@/shared/products";
import * as fx from "@/shared/test/fixtures";
import { renderPortfolioPage } from "./portfolio.test-utils";

/**
 * "Add to portfolio" in the workbench lands on /portfolio?trade=…: the trade
 * form opens on the library product with the workbench's terms (WP 28, lot
 * U2; issue 313, lot b3).
 */
const KEY = "qm_local_portfolios";

describe("a trade carried in from the workbench", () => {
	beforeEach(() =>
		localStorage.setItem(KEY, JSON.stringify([fx.portfolio("Long book")])),
	);
	afterEach(() => localStorage.clear());

	it("opens the trade form on the product, with its terms", async () => {
		const trade = encodeParams({
			product: "script-up-and-out-call",
			values: { barrier: 123.5, tickers: "SPY" },
		});
		renderPortfolioPage(`/portfolio?trade=${trade}`);
		const type = await screen.findByRole("combobox", {
			name: "Instrument type",
		});
		expect(type).toHaveValue("script");
		expect(await screen.findByDisplayValue("123.5")).toBeInTheDocument();
		expect(
			screen.getByRole("option", { name: /up-and-out/i, selected: true }),
		).toBeInTheDocument();
	});

	it("waits for a portfolio to be created, then opens the form", async () => {
		localStorage.clear();
		const trade = encodeParams({ product: "script-digital-call", values: {} });
		renderPortfolioPage(`/portfolio?trade=${trade}`);
		expect(
			await screen.findByText(/opens on the product from the workbench/),
		).toBeInTheDocument();
		fireEvent.click(screen.getByRole("button", { name: "Create portfolio" }));
		expect(
			await screen.findByRole("combobox", { name: "Instrument type" }),
		).toHaveValue("script");
	});

	it("leaves the form closed without one", async () => {
		renderPortfolioPage();
		await screen.findByRole("heading", { name: "Long book" });
		expect(
			screen.queryByRole("combobox", { name: "Instrument type" }),
		).toBeNull();
	});
});
