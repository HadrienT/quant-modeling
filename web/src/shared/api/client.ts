import createClient, { type Middleware } from "openapi-fetch";
import { getConfig } from "@/shared/config";
import { toApiError } from "./errors";
import type { paths } from "./schema.gen";

/**
 * The single typed request surface — blueprint WP 02 §2.
 * No component imports fetch; everything goes through `api`.
 *
 * - base URL from runtime config (not import.meta.env, never VITE_API_KEY)
 * - bearer token injected in exactly one place (auth middleware)
 * - AbortSignal always propagated (openapi-fetch forwards `signal`)
 * - non-2xx responses thrown as a normalised ApiError
 */

export const DEFAULT_TIMEOUT_MS = 15_000;
/** Monte-Carlo pricing / portfolio VaR can legitimately run long. */
export const LONG_TIMEOUT_MS = 120_000;

let authToken: string | null = null;
export function setAuthToken(token: string | null) {
	authToken = token;
}
export function getAuthToken() {
	return authToken;
}

const authAndErrorMiddleware: Middleware = {
	onRequest({ request }) {
		if (authToken) request.headers.set("Authorization", `Bearer ${authToken}`);
		return request;
	},
	async onResponse({ response }) {
		if (response.ok) return response;
		let body: unknown = null;
		try {
			body = await response.clone().json();
		} catch {
			try {
				body = { message: await response.clone().text() };
			} catch {
				body = null;
			}
		}
		throw toApiError(response.status, body);
	},
};

export const api = createClient<paths>({
	get baseUrl() {
		// Empty runtime config = same origin (nginx serves API + app together).
		return (
			getConfig().apiBase ||
			(typeof window !== "undefined" ? window.location.origin : "")
		);
	},
	// Resolve fetch lazily so MSW (which swaps globalThis.fetch) is picked up.
	fetch: (...args: Parameters<typeof fetch>) => globalThis.fetch(...args),
});
api.use(authAndErrorMiddleware);

/**
 * A signal that aborts when `external` aborts OR after `timeoutMs`.
 * Pass the result as `signal` to any `api.*` call. Call `.cleanup()` when done
 * (e.g. in a finally) to clear the timer.
 */
export function signalWithTimeout(
	external: AbortSignal | undefined,
	timeoutMs: number = DEFAULT_TIMEOUT_MS,
): AbortSignal & { cleanup: () => void } {
	const ctrl = new AbortController();
	const timer = setTimeout(
		() => ctrl.abort(new DOMException("timeout", "TimeoutError")),
		timeoutMs,
	);
	const onAbort = () => ctrl.abort(external?.reason);
	if (external) {
		if (external.aborted) onAbort();
		else external.addEventListener("abort", onAbort);
	}
	const signal = ctrl.signal as AbortSignal & { cleanup: () => void };
	signal.cleanup = () => {
		clearTimeout(timer);
		external?.removeEventListener("abort", onAbort);
	};
	return signal;
}
