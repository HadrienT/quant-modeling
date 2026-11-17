import "@testing-library/jest-dom/vitest";
import { toHaveNoViolations } from "jest-axe";
import { afterAll, afterEach, beforeAll, expect } from "vitest";
import { __resetMarketContext } from "@/shared/session/marketContext";
import { server } from "@/shared/test/msw-server";

expect.extend(toHaveNoViolations);

beforeAll(() => server.listen({ onUnhandledRequest: "error" }));
afterEach(() => {
	server.resetHandlers();
	// The underlying shared by the pages (WP 28, U4) must not leak between
	// tests: one that visits ?ticker= would change the next one's default.
	try {
		localStorage.removeItem("qm_market_context");
	} catch {
		/* no storage */
	}
	__resetMarketContext();
});
afterAll(() => server.close());

// jsdom lacks these; several UI primitives (Radix) and charts need them.
if (!window.matchMedia) {
	window.matchMedia = ((query: string) => ({
		matches: false,
		media: query,
		onchange: null,
		addListener: () => {},
		removeListener: () => {},
		addEventListener: () => {},
		removeEventListener: () => {},
		dispatchEvent: () => false,
	})) as typeof window.matchMedia;
}

if (!window.ResizeObserver) {
	window.ResizeObserver = class {
		observe() {}
		unobserve() {}
		disconnect() {}
	};
}

// jsdom does not implement these; TanStack Router scroll restoration calls them.
window.scrollTo = () => {};
if (!Element.prototype.scrollIntoView) {
	Element.prototype.scrollIntoView = () => {};
}
