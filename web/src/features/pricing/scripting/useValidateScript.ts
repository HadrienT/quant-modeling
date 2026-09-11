import { useMutation } from "@tanstack/react-query";
import { api, ApiError, signalWithTimeout } from "@/shared/api";
import { LONG_TIMEOUT_MS } from "@/shared/api/client";
import type { DayCount } from "../ScriptingMarketFields";

/**
 * Parse-only companion to pricing a script: POST /price/scripted/validate,
 * no market inputs, no simulation. Meant to run on every "Validate" click —
 * cheap enough that there is no reason to debounce it onto every keystroke.
 */
export function useValidateScript() {
	return useMutation({
		mutationFn: async (body: {
			script: string;
			valuation_date: string;
			day_count: DayCount;
		}) => {
			const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
			try {
				const { data, error } = await api.POST("/price/scripted/validate", {
					body,
					signal: s,
				});
				if (error !== undefined) throw ApiError.from(error);
				return data;
			} finally {
				s.cleanup();
			}
		},
	});
}
