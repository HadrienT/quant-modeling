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

type PagePath = NavPage["path"];

/**
 * The four spaces of the navigation (WP 28), in the order a desk works: read
 * the market, price a product, risk a book, learn how it works. `NAV_PAGES`
 * stays the flat list the API mirrors; a group only references its pages.
 */
export const NAV_GROUPS: readonly {
	key: string;
	label: string;
	question: string;
	pages: readonly PagePath[];
}[] = [
	{
		key: "market",
		label: "Market",
		question: "What does the market say?",
		pages: ["/market", "/credit"],
	},
	{
		key: "price",
		label: "Price",
		question: "What is one product worth?",
		pages: ["/price", "/scripting", "/visualize"],
	},
	{
		key: "risk",
		label: "Risk",
		question: "What does a book cost and risk?",
		pages: ["/portfolio", "/xva", "/backtest"],
	},
	{
		key: "learn",
		label: "Learn",
		question: "How does it work?",
		pages: ["/products", "/simulation", "/about"],
	},
];

/** One line per page, shown in the navigation menus and the command palette. */
export const PAGE_HINTS: Record<PagePath, string> = {
	"/market": "Prices, volatility surfaces, rate curves, FX",
	"/credit": "Spreads by rating, hazard rates, company statements",
	"/price": "The product catalogue under every engine, with greeks",
	"/scripting": "Write a payoff, price it on CPU or GPU",
	"/visualize": "Build an option strategy leg by leg",
	"/portfolio": "Book trades, mark them daily, follow P&L and risk",
	"/xva": "Exposure, CVA, DVA, FVA, margin and capital",
	"/backtest": "An allocation strategy on real prices, net of costs",
	"/products": "Each product: payoff, assumptions, sources",
	"/simulation": "Paths of calibrated diffusion models",
	"/about": "How the site is built",
};
