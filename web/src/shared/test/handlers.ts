import { HttpResponse, http } from "msw";
import * as fx from "./fixtures";

/**
 * MSW handlers — serve the SAME fixtures to Vitest, Storybook and offline dev
 * (blueprint WP 02 §4, WP 12 pitfalls). `*` prefix so any base URL matches.
 */

const portfolios = new Map<string, ReturnType<typeof fx.portfolio>>();
const seed = fx.portfolio();
portfolios.set(seed.id, seed);

export const handlers = [
	http.get("*/health", () =>
		HttpResponse.json({ status: "ok", version: "mock" }),
	),

	http.get("*/market/tickers", ({ request }) =>
		HttpResponse.json(
			fx.marketTickers(new URL(request.url).searchParams.get("market")),
		),
	),

	http.get("*/market/fx/overview", ({ request }) => {
		const u = new URL(request.url).searchParams;
		return HttpResponse.json(
			fx.fxOverview(u.get("base") ?? "EUR", u.get("quote") ?? "USD"),
		);
	}),

	http.get("*/market/fx/history", ({ request }) => {
		const u = new URL(request.url).searchParams;
		return HttpResponse.json(
			fx.fxHistory(u.get("base") ?? "EUR", u.get("quote") ?? "USD"),
		);
	}),

	http.get("*/market/fx/correlation", ({ request }) =>
		HttpResponse.json(
			fx.fxCorrelation(new URL(request.url).searchParams.get("ticker") ?? ""),
		),
	),

	http.get("*/market/markets", () =>
		HttpResponse.json({ markets: fx.MARKETS }),
	),

	http.get("*/market/prices/history", ({ request }) => {
		const url = new URL(request.url);
		const ticker = url.searchParams.get("ticker") ?? "AAPL";
		return HttpResponse.json(fx.priceHistory(ticker));
	}),

	http.get("*/api/local-vol/raw-surface", ({ request }) => {
		const url = new URL(request.url);
		return HttpResponse.json(
			fx.rawIvSurface(url.searchParams.get("ticker") ?? "AAPL"),
		);
	}),

	http.get("*/api/local-vol/iv-surface", ({ request }) => {
		const url = new URL(request.url);
		return HttpResponse.json(
			fx.cleanedIvSurface(url.searchParams.get("ticker") ?? "AAPL"),
		);
	}),

	http.get("*/api/local-vol/surface", ({ request }) => {
		const url = new URL(request.url);
		return HttpResponse.json(
			fx.localVolSurface(url.searchParams.get("ticker") ?? "AAPL"),
		);
	}),

	http.get("*/market/rates/overview", ({ request }) => {
		const url = new URL(request.url);
		return HttpResponse.json(
			fx.ratesOverview(url.searchParams.get("currency") ?? "USD"),
		);
	}),

	http.get("*/market/rates/history", ({ request }) => {
		const url = new URL(request.url);
		return HttpResponse.json(
			fx.ratesHistory(
				url.searchParams.get("currency") ?? "USD",
				url.searchParams.get("series_id") ?? "ON",
			),
		);
	}),

	http.post("*/price/*", () => HttpResponse.json(fx.pricingResponse())),

	http.get("*/api/portfolios", () =>
		HttpResponse.json(
			[...portfolios.values()].map((p) => ({
				id: p.id,
				name: p.name,
				created_at: p.created_at,
				updated_at: p.updated_at,
				n_positions: p.instruments.length,
				total_value: 0,
			})),
		),
	),

	http.get("*/api/portfolios/:id", ({ params }) => {
		const p = portfolios.get(String(params.id));
		return p
			? HttpResponse.json(p)
			: HttpResponse.json(
					{ code: "not_found", message: "Portfolio not found" },
					{ status: 404 },
				);
	}),

	http.delete("*/api/portfolios/:id", ({ params }) =>
		portfolios.delete(String(params.id))
			? new HttpResponse(null, { status: 204 })
			: HttpResponse.json(
					{ code: "not_found", message: "Portfolio not found" },
					{ status: 404 },
				),
	),

	http.put("*/api/portfolios/:id", async ({ params, request }) => {
		const body = (await request.json()) as ReturnType<typeof fx.portfolio>;
		const saved = { ...body, id: String(params.id) };
		portfolios.set(saved.id, saved);
		return HttpResponse.json(saved);
	}),

	http.get("*/api/portfolio-valuation/demos", () =>
		HttpResponse.json(fx.portfolioDemos()),
	),
	http.post("*/api/portfolio-valuation/snapshot", () =>
		HttpResponse.json(fx.portfolioSnapshot()),
	),
	http.post("*/api/portfolio-valuation/history", () =>
		HttpResponse.json(fx.portfolioHistory()),
	),
	http.get("*/api/portfolio-valuation/close", ({ request }) => {
		const u = new URL(request.url).searchParams;
		return HttpResponse.json({
			ticker: u.get("ticker"),
			date: u.get("date"),
			close: 440,
			currency: "EUR",
		});
	}),

	http.post("*/api/backtest/run", () =>
		HttpResponse.json(fx.backtestResponse()),
	),

	http.post("*/api/auth/login", async ({ request }) => {
		const body = (await request.json()) as { username?: string };
		return HttpResponse.json({
			token: "mock.jwt.token",
			username: body.username ?? "demo",
		});
	}),
	http.post("*/api/auth/register", async ({ request }) => {
		const body = (await request.json()) as { username?: string };
		return HttpResponse.json(
			{ token: "mock.jwt.token", username: body.username ?? "demo" },
			{ status: 201 },
		);
	}),
	http.get("*/api/auth/me", ({ request }) => {
		const auth = request.headers.get("Authorization");
		return auth
			? HttpResponse.json({
					username: "google:1234567890",
					email: "alice@example.com",
					provider: "google",
					created_at: "2026-09-19T10:00:00+00:00",
				})
			: HttpResponse.json(
					{ code: "unauthorized", message: "Invalid token" },
					{ status: 401 },
				);
	}),
];
