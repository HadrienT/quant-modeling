/**
 * One error shape for the whole client — blueprint WP 02 §2.
 * Distinguishes network / 4xx / 5xx / abort and carries a presentable message
 * plus the technical detail for logs.
 */

export type ApiErrorKind =
	"network" | "timeout" | "abort" | "client" | "server";

export class ApiError extends Error {
	readonly kind: ApiErrorKind;
	readonly status: number;
	readonly code: string;
	readonly detail: unknown;

	constructor(init: {
		kind: ApiErrorKind;
		status: number;
		code: string;
		message: string;
		detail?: unknown;
	}) {
		super(init.message);
		this.name = "ApiError";
		this.kind = init.kind;
		this.status = init.status;
		this.code = init.code;
		this.detail = init.detail;
	}

	/** True for errors where an automatic retry could plausibly help: the
	 * network, a timeout, a 5xx, and a 429 "busy" (every pricing slot taken,
	 * issue 347) -- not a 429 that a retry cannot change, such as a visitor's
	 * exhausted daily quota. */
	get retriable(): boolean {
		return (
			this.kind === "network" ||
			this.kind === "timeout" ||
			this.status >= 500 ||
			this.busy
		);
	}

	/** The server had no free slot for the request: worth retrying in a few
	 * seconds. */
	get busy(): boolean {
		return this.status === 429 && this.code !== "anonymous_quota_exhausted";
	}

	static from(error: unknown): ApiError {
		if (error instanceof ApiError) return error;
		if (error instanceof DOMException && error.name === "AbortError") {
			return new ApiError({
				kind: "abort",
				status: 0,
				code: "aborted",
				message: "Request cancelled.",
			});
		}
		return new ApiError({
			kind: "network",
			status: 0,
			code: "network",
			message:
				error instanceof Error ? error.message : "Network request failed.",
			detail: error,
		});
	}
}

/** Shape the FastAPI error handler returns (see api/app/main.py ApiError). */
type ServerError = { code?: string; message?: string; detail?: unknown };

export function toApiError(status: number, body: unknown): ApiError {
	const server = (body ?? {}) as ServerError;
	const kind: ApiErrorKind = status >= 500 ? "server" : "client";
	const fallback =
		status >= 500
			? "The server failed to handle the request."
			: "The request was rejected.";
	return new ApiError({
		kind,
		status,
		code: server.code ?? `http_${status}`,
		message: server.message ?? fallback,
		detail: server.detail ?? body,
	});
}
