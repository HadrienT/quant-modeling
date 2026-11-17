import { act, renderHook } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { rememberTicker, useMarketContext } from "./marketContext";

/** The underlying the pages start on (WP 28, lot U4). */
describe("market context", () => {
	afterEach(() => vi.restoreAllMocks());

	it("is empty until an underlying is chosen", () => {
		const { result } = renderHook(() => useMarketContext());
		expect(result.current).toBeNull();
	});

	it("remembers the last choice, upper-cased, and tells the pages", () => {
		const { result } = renderHook(() => useMarketContext());
		act(() => rememberTicker("msft"));
		expect(result.current).toEqual({ ticker: "MSFT", market: "SP500" });
		act(() => rememberTicker("MC.PA", "CAC40"));
		expect(result.current).toEqual({ ticker: "MC.PA", market: "CAC40" });
		expect(JSON.parse(localStorage.getItem("qm_market_context")!)).toEqual({
			ticker: "MC.PA",
			market: "CAC40",
		});
	});

	it("survives blocked storage, kept in memory for the tab", () => {
		vi.spyOn(Storage.prototype, "getItem").mockImplementation(() => {
			throw new Error("blocked");
		});
		vi.spyOn(Storage.prototype, "setItem").mockImplementation(() => {
			throw new Error("blocked");
		});
		const { result } = renderHook(() => useMarketContext());
		act(() => rememberTicker("NVDA"));
		expect(result.current).toEqual({ ticker: "NVDA", market: "SP500" });
	});

	it("ignores a corrupt entry", () => {
		localStorage.setItem("qm_market_context", "{not json");
		const { result } = renderHook(() => useMarketContext());
		expect(result.current).toBeNull();
	});
});
