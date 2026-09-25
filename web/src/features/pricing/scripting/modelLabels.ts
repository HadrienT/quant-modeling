import type { ScriptModel } from "@/shared/api";

/** How each scripting model is named on the page. */
export const MODEL_LABELS: Record<ScriptModel, string> = {
	auto: "Auto — the model this script needs",
	black_scholes: "Black-Scholes (flat vol)",
	local_vol: "Local vol (market surface)",
	heston: "Heston (calibrated)",
	slv: "Stochastic-local vol (calibrated)",
};
