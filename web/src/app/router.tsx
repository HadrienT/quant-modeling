import { lazy } from "react";
import {
	createRootRoute,
	createRoute,
	createRouter,
	redirect,
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

const indexRoute = createRoute({
	getParentRoute: () => rootRoute,
	path: "/",
	beforeLoad: () => {
		throw redirect({ to: "/visualize" });
	},
});

// ── Strategies (home) ────────────────────────────────────────────────────
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
		tab: z.enum(["prices", "vol", "rates"]).default("prices"),
		ticker: z.string().optional(),
		surface: z.enum(["raw", "cleaned", "localvol"]).optional(),
	}).parse,
	component: MarketPage,
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
	validateSearch: z.object({ id: z.string().optional() }).parse,
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

const routeTree = rootRoute.addChildren([
	indexRoute,
	visualizeRoute,
	marketRoute,
	priceRoute,
	datedAsianRoute,
	scriptingRoute,
	productsRoute,
	portfolioRoute,
	backtestRoute,
	aboutRoute,
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
	{ path: "/price", label: "Pricing" },
	{ path: "/dated-asian", label: "Dated Asian" },
	{ path: "/scripting", label: "Scripting" },
	{ path: "/products", label: "Products" },
	{ path: "/portfolio", label: "Portfolio" },
	{ path: "/backtest", label: "Backtest" },
	{ path: "/about", label: "About" },
] as const;
