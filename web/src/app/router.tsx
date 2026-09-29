import { lazy } from "react";
import {
	createRootRoute,
	createRoute,
	createRouter,
} from "@tanstack/react-router";
import { z } from "zod";
import { AppLayout } from "./layout/AppLayout";
import { RouteError } from "./layout/RouteError";
import { NotFound } from "./layout/NotFound";

/**
 * Code-based routing (TanStack Router, ADR-007). One lazy module per route.
 * Search params are validated by zod and typed end to end — a pricing session,
 * a ticker + surface tab, a backtest config all become shareable URLs.
 *
 * ADR-004 guard rail: until a page is migrated, its legacy component is mounted
 * here unchanged, so the app is never broken.
 */

const rootRoute = createRootRoute({
	component: AppLayout,
	errorComponent: RouteError,
	notFoundComponent: NotFound,
});

// ── Home ─────────────────────────────────────────────────────────────────
const HomePage = lazy(() => import("@/features/home/HomePage"));
const indexRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/",
	component: HomePage,
});

// ── Strategies ───────────────────────────────────────────────────────────
const StrategiesPage = lazy(
	() => import("@/features/strategies/StrategiesPage"),
);
const visualizeRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/visualize",
	validateSearch: z.object({ s: z.string().optional() }).parse,
	component: StrategiesPage,
});

// ── Market ───────────────────────────────────────────────────────────────
const MarketPage = lazy(() => import("@/features/market/MarketPage"));
const marketRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/market",
	validateSearch: z.object({
		tab: z.enum(["prices", "vol", "rates", "fx"]).default("prices"),
		ticker: z.string().optional(),
		surface: z.enum(["raw", "cleaned", "localvol"]).optional(),
		ccy: z.enum(["USD", "EUR", "GBP", "CHF", "JPY"]).optional(),
		market: z
			.enum(["SP500", "CAC40", "DAX", "FTSE100", "NIKKEI225"])
			.optional(),
		base: z.enum(["USD", "EUR", "GBP", "JPY", "CHF"]).optional(),
		quote: z.enum(["USD", "EUR", "GBP", "JPY", "CHF"]).optional(),
	}).parse,
	component: MarketPage,
});

// ── Credit ───────────────────────────────────────────────────────────────
// Page at /credit; its API lives under /api/credit/*, so the dev proxy and
// nginx need no new prefix and never shadow the page.
const CreditPage = lazy(() => import("@/features/credit/CreditPage"));
const creditRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/credit",
	validateSearch: z.object({
		tab: z.enum(["spreads", "companies"]).default("spreads"),
		ticker: z.string().optional(),
		freq: z.enum(["annual", "quarterly"]).optional(),
		recovery: z.number().min(0).max(0.9).optional(),
	}).parse,
	component: CreditPage,
});

// ── Pricing ──────────────────────────────────────────────────────────────
const PricingPage = lazy(() => import("@/features/pricing/PricingPage"));
const priceRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/price",
	validateSearch: z.object({
		product: z.string().optional(),
		engine: z.string().optional(),
		p: z.string().optional(), // encoded params
		compare: z.string().optional(),
		device: z.enum(["cpu", "gpu", "auto"]).optional(),
	}).parse,
	component: PricingPage,
});

// ── Dated Asian (preview of the timeline / calendar engine) ──────────────
// Not under /price/* — the Vite dev proxy sends every /price/<...> path to the
// API, so an SPA route there is unreachable in dev.
const DatedAsianPreview = lazy(
	() => import("@/features/pricing/DatedAsianPreview"),
);
const datedAsianRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/dated-asian",
	component: DatedAsianPreview,
});

// ── Simulation ───────────────────────────────────────────────────────────
const SimulationPage = lazy(
	() => import("@/features/simulation/SimulationPage"),
);
const simulationRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/simulation",
	component: SimulationPage,
});

// ── Payoff scripting (preview of the WP 16 language) ──────────────────────
// Not under /price/* — same dev-proxy reason as the Dated Asian route above.
const ScriptingPreview = lazy(
	() => import("@/features/pricing/ScriptingPreview"),
);
const scriptingRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/scripting",
	component: ScriptingPreview,
});

// ── Products (reference) ─────────────────────────────────────────────────
const ProductsPage = lazy(() => import("@/features/products/ProductsPage"));
const productsRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/products",
	validateSearch: z.object({ product: z.string().optional() }).parse,
	component: ProductsPage,
});

// ── Portfolio ────────────────────────────────────────────────────────────
const PortfolioPage = lazy(() => import("@/features/portfolio/PortfolioPage"));
const portfolioRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/portfolio",
	validateSearch: z.object({
		id: z.string().optional(),
		demo: z.string().optional(),
	}).parse,
	component: PortfolioPage,
});

// ── Backtest ─────────────────────────────────────────────────────────────
const BacktestPage = lazy(() => import("@/features/backtest/BacktestPage"));
const backtestRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/backtest",
	validateSearch: z.object({ c: z.string().optional() }).parse,
	component: BacktestPage,
});

// ── About ────────────────────────────────────────────────────────────────
const AboutPage = lazy(() => import("@/features/about/AboutPage"));
const aboutRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/about",
	component: AboutPage,
});

// ── Legal pages (from the footer, not in the nav) ────────────────────────
const LegalNoticePage = lazy(() => import("@/features/legal/LegalNoticePage"));
const PrivacyPage = lazy(() => import("@/features/legal/PrivacyPage"));
const TermsPage = lazy(() => import("@/features/legal/TermsPage"));
const legalRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/legal",
	component: LegalNoticePage,
});
const privacyRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/privacy",
	component: PrivacyPage,
});
const termsRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/terms",
	component: TermsPage,
});

// ── Profile (from the avatar menu, not in the nav) ───────────────────────
const ProfilePage = lazy(() => import("@/features/auth/ProfilePage"));
const profileRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/profile",
	component: ProfilePage,
});

const routeTree = rootRoute.addChildren([
	indexRoute,
	visualizeRoute,
	marketRoute,
	creditRoute,
	priceRoute,
	datedAsianRoute,
	simulationRoute,
	scriptingRoute,
	productsRoute,
	portfolioRoute,
	backtestRoute,
	aboutRoute,
	legalRoute,
	privacyRoute,
	termsRoute,
	profileRoute,
]);

export const router = createRouter({
	routeTree,
	defaultPreload: "intent",
	defaultPreloadStaleTime: 0,
	scrollRestoration: true,
});

declare module "@tanstack/react-router" {
	interface Register {
		router: typeof router;
	}
}

export const ROUTES = [
	{ path: "/visualize", label: "Strategies" },
	{ path: "/market", label: "Market" },
	{ path: "/credit", label: "Credit" },
	{ path: "/price", label: "Pricing" },
	{ path: "/simulation", label: "Simulation" },
	{ path: "/scripting", label: "Scripting" },
	{ path: "/products", label: "Products" },
	{ path: "/portfolio", label: "Portfolio" },
	{ path: "/backtest", label: "Backtest" },
	{ path: "/about", label: "About" },
] as const;

/** Browser-tab titles; a path missing here gets the bare site name. */
export const PAGE_TITLES: Record<string, string> = {
	...Object.fromEntries(ROUTES.map((r) => [r.path, r.label])),
	"/dated-asian": "Dated Asian",
	"/legal": "Legal notice",
	"/privacy": "Privacy policy",
	"/terms": "Terms & disclaimer",
	"/profile": "Profile",
};
