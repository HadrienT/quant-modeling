import { useSyncExternalStore } from "react";

/**
 * Theme control — blueprint WP 03 §4, ADR-009.
 * Dark by default; prefers-color-scheme on first load; explicit choice persisted.
 * The blocking script in index.html sets data-theme before first paint; this
 * module keeps it in sync afterwards and notifies the WebGL bridge implicitly
 * (tokens.ts observes the data-theme attribute).
 */

export type Theme = "dark" | "light";
const KEY = "qm-theme";

function readStored(): Theme | null {
	try {
		const v = localStorage.getItem(KEY);
		return v === "dark" || v === "light" ? v : null;
	} catch {
		return null;
	}
}

function systemTheme(): Theme {
	return window.matchMedia?.("(prefers-color-scheme: dark)").matches
		? "dark"
		: "light";
}

let current: Theme = readStored() ?? systemTheme();
const listeners = new Set<() => void>();

function apply(theme: Theme) {
	current = theme;
	document.documentElement.setAttribute("data-theme", theme);
	listeners.forEach((l) => l());
}

// Reflect the initial resolved value onto <html> (covers the "system" case
// where the blocking script left the attribute unset).
if (typeof document !== "undefined") {
	document.documentElement.setAttribute("data-theme", current);
}

export function setTheme(theme: Theme) {
	try {
		localStorage.setItem(KEY, theme);
	} catch {
		/* private mode */
	}
	apply(theme);
}

export function toggleTheme() {
	setTheme(current === "dark" ? "light" : "dark");
}

export function useTheme(): Theme {
	return useSyncExternalStore(
		(cb) => {
			listeners.add(cb);
			return () => listeners.delete(cb);
		},
		() => current,
		() => "dark" as Theme,
	);
}
