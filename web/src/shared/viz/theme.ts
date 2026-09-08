import { useSyncExternalStore } from "react";
import {
	type ThemeTokens,
	onThemeTokensChange,
	readThemeTokens,
} from "@/shared/styles/tokens";

/**
 * Chart theme — blueprint WP 06 §3. Both engines read the SAME tokens as the 3D
 * renderer, so a series blue is the same blue everywhere. lightweight-charts has
 * its own theme system and must be PUSHED these values on every theme change.
 */

let snapshot = typeof document !== "undefined" ? readThemeTokens() : fallback();
let started = false;
const listeners = new Set<() => void>();

function fallback(): ThemeTokens {
	return {
		canvas: "#0b0e13",
		surface: "#141922",
		surfaceRaised: "#1b212c",
		ink: "#f2f5f8",
		inkSecondary: "#a9b4c2",
		inkMuted: "#7d8899",
		hairline: "#212936",
		axis: "#38414f",
		accent: "#3987e5",
		series: [
			"#3987e5",
			"#d95926",
			"#199e70",
			"#c98500",
			"#d55181",
			"#008300",
			"#9085e9",
			"#e66767",
		],
		sequential: ["#cde2fb", "#7fb2ee", "#3987e5", "#1f5bb0", "#0d366b"],
		divergent: ["#2a78d6", "#d03b3b"],
		diverneutral: "#7d8899",
		status: {
			good: "#0ca30c",
			warning: "#fab219",
			serious: "#ec835a",
			critical: "#d03b3b",
		},
		pnl: { up: "#0ca30c", down: "#d03b3b" },
	};
}

function ensureStarted() {
	if (started || typeof document === "undefined") return;
	started = true;
	onThemeTokensChange((tokens) => {
		snapshot = tokens;
		listeners.forEach((l) => l());
	});
}

export function useChartTheme(): ThemeTokens {
	ensureStarted();
	return useSyncExternalStore(
		(cb) => {
			listeners.add(cb);
			return () => listeners.delete(cb);
		},
		() => snapshot,
		() => fallback(),
	);
}

/** Non-hook read (imperative canvas code). */
export function chartTheme(): ThemeTokens {
	return typeof document !== "undefined" ? readThemeTokens() : fallback();
}
