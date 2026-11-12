import { LONG_TIMEOUT_MS, signalWithTimeout } from "./client";
import { ApiError } from "./errors";

/**
 * One call of a simulation endpoint (xVA, rates derivatives, backtest): a
 * few seconds of computation, so the long timeout; an error body becomes an
 * ApiError.
 */
export async function longCall<T>(
	signal: AbortSignal | undefined,
	run: (s: AbortSignal) => Promise<{ data?: unknown; error?: unknown }>,
): Promise<T> {
	const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
	try {
		const { data, error } = await run(s);
		if (error !== undefined) throw ApiError.from(error);
		return data as T;
	} finally {
		s.cleanup();
	}
}
