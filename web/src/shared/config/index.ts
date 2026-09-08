/**
 * Runtime configuration — blueprint WP 14.
 *
 * Vite inlines VITE_* at build time, so an image cannot be re-pointed at a new
 * API without a rebuild, and VITE_API_KEY would leak into the bundle (ADR-008).
 * Instead the server serves /config.json (or injects window.__APP_CONFIG__).
 * import.meta.env is only a dev fallback and never carries a secret.
 */

export type AppConfig = {
	apiBase: string;
	commitSha: string;
};

declare global {
	interface Window {
		__APP_CONFIG__?: Partial<AppConfig>;
	}
}

const FALLBACK: AppConfig = {
	apiBase: import.meta.env.VITE_API_BASE ?? "",
	commitSha: import.meta.env.VITE_COMMIT_SHA ?? "dev",
};

let cache: AppConfig | null = null;

export async function loadConfig(): Promise<AppConfig> {
	if (cache) return cache;

	const injected = window.__APP_CONFIG__;
	if (injected?.apiBase !== undefined) {
		cache = { ...FALLBACK, ...injected } as AppConfig;
		return cache;
	}

	try {
		const res = await fetch("/config.json", { cache: "no-store" });
		if (res.ok) {
			const body = (await res.json()) as Partial<AppConfig>;
			cache = { ...FALLBACK, ...body };
			return cache;
		}
	} catch {
		/* no config.json in dev — fall through */
	}

	cache = FALLBACK;
	return cache;
}

/** Synchronous access after loadConfig() has resolved once. */
export function getConfig(): AppConfig {
	return cache ?? FALLBACK;
}
