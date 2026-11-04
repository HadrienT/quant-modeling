/**
 * The pages of the navigation, in order. An administrator can hide any of
 * them from visitors (admin panel, `api/app/site_pages.py`): the API keeps
 * the same list, and `api/tests/test_site_pages.py` checks that the two agree.
 */
export const NAV_PAGES = [
	{ path: "/price", label: "Pricing" },
	{ path: "/scripting", label: "Scripting" },
	{ path: "/market", label: "Market" },
	{ path: "/credit", label: "Credit" },
	{ path: "/xva", label: "xVA" },
	{ path: "/simulation", label: "Simulation" },
	{ path: "/portfolio", label: "Portfolio" },
	{ path: "/backtest", label: "Backtest" },
	{ path: "/visualize", label: "Strategies" },
	{ path: "/products", label: "Products" },
	{ path: "/about", label: "About" },
] as const;

export type NavPage = (typeof NAV_PAGES)[number];
