import { http, HttpResponse } from "msw";

/**
 * MSW request handlers. Fleshed out in WP 02 with fixtures derived from
 * schema.gen.ts. For now only /health so the harness has something green.
 */
export const handlers = [
	http.get("*/health", () =>
		HttpResponse.json({ status: "ok", version: "test" }),
	),
];
