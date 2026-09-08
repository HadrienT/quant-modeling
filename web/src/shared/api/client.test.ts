import { http, HttpResponse, delay } from "msw";
import { afterEach, describe, expect, it } from "vitest";
import { server } from "@/shared/test/msw-server";
import { api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";

afterEach(() => server.resetHandlers());

describe("api client", () => {
	it("normalises a 500 into a readable ApiError, not a raw JSON string", async () => {
		server.use(
			http.get("*/health", () =>
				HttpResponse.json(
					{ code: "boom", message: "Database unavailable" },
					{ status: 500 },
				),
			),
		);
		await expect(api.GET("/health", {})).rejects.toMatchObject({
			name: "ApiError",
			code: "boom",
			message: "Database unavailable",
		});
	});

	it("throws ApiError with kind 'server' on a 5xx", async () => {
		server.use(
			http.get("*/health", () =>
				HttpResponse.json({ message: "nope" }, { status: 503 }),
			),
		);
		await expect(api.GET("/health", {})).rejects.toMatchObject({
			name: "ApiError",
			kind: "server",
			status: 503,
			message: "nope",
		});
	});

	it("aborts an in-flight request when its signal fires", async () => {
		server.use(
			http.get("*/market/tickers", async () => {
				await delay(1000);
				return HttpResponse.json({ tickers: [] });
			}),
		);
		const controller = new AbortController();
		const signal = signalWithTimeout(controller.signal, 60_000);
		const promise = api.GET("/market/tickers", { signal });
		controller.abort();
		await expect(promise).rejects.toBeDefined();
		signal.cleanup();
	});

	it("times out a slow request", async () => {
		server.use(
			http.get("*/market/tickers", async () => {
				await delay(200);
				return HttpResponse.json({ tickers: [] });
			}),
		);
		const signal = signalWithTimeout(undefined, 10);
		await expect(api.GET("/market/tickers", { signal })).rejects.toBeDefined();
		signal.cleanup();
	});
});

describe("ApiError", () => {
	it("marks network and 5xx as retriable, 4xx as not", () => {
		expect(
			new ApiError({ kind: "server", status: 500, code: "x", message: "" })
				.retriable,
		).toBe(true);
		expect(
			new ApiError({ kind: "client", status: 400, code: "x", message: "" })
				.retriable,
		).toBe(false);
	});
});
