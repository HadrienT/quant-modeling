import { api, signalWithTimeout } from "@/shared/api";
import type { Portfolio, PortfolioSummary, Position } from "@/shared/api";

/**
 * PortfolioRepository port — blueprint WP 04 §4. Portfolio components talk to
 * this, never to the session. Two implementations: local (anonymous, this
 * device) and server (authenticated).
 */
export interface PortfolioRepository {
	readonly kind: "local" | "server";
	list(): Promise<PortfolioSummary[]>;
	get(id: string): Promise<Portfolio | null>;
	create(name: string): Promise<Portfolio>;
	rename(id: string, name: string): Promise<void>;
	remove(id: string): Promise<void>;
	putPositions(id: string, positions: Position[]): Promise<Portfolio>;
	/** import a full portfolio object (JSON round-trip) */
	importPortfolio(pf: Portfolio): Promise<Portfolio>;
}

/* ── Local (localStorage) ──────────────────────────────────────────────── */

const LS_KEY = "qm_local_portfolios";
const now = () => new Date().toISOString();
const uid = () =>
	crypto.randomUUID?.() ?? Math.random().toString(36).slice(2, 14);

function loadAll(): Portfolio[] {
	try {
		return JSON.parse(localStorage.getItem(LS_KEY) ?? "[]") as Portfolio[];
	} catch {
		return [];
	}
}
function saveAll(all: Portfolio[]) {
	try {
		localStorage.setItem(LS_KEY, JSON.stringify(all));
	} catch {
		/* private mode */
	}
}
function summarise(pf: Portfolio): PortfolioSummary {
	const positions = pf.positions ?? [];
	return {
		id: pf.id,
		name: pf.name,
		created_at: pf.created_at ?? "",
		updated_at: pf.updated_at ?? "",
		n_positions: positions.length,
		total_value: positions.reduce((s, p) => s + (p.result?.npv ?? 0), 0),
	};
}

export const localRepository: PortfolioRepository = {
	kind: "local",
	async list() {
		return loadAll().map(summarise);
	},
	async get(id) {
		return loadAll().find((p) => p.id === id) ?? null;
	},
	async create(name) {
		const pf: Portfolio = {
			id: uid(),
			name,
			owner: "",
			created_at: now(),
			updated_at: now(),
			positions: [],
		};
		saveAll([...loadAll(), pf]);
		return pf;
	},
	async rename(id, name) {
		saveAll(
			loadAll().map((p) =>
				p.id === id ? { ...p, name, updated_at: now() } : p,
			),
		);
	},
	async remove(id) {
		saveAll(loadAll().filter((p) => p.id !== id));
	},
	async putPositions(id, positions) {
		const all = loadAll();
		const pf = all.find((p) => p.id === id);
		if (!pf) throw new Error("Portfolio not found");
		pf.positions = positions;
		pf.updated_at = now();
		saveAll(all);
		return { ...pf };
	},
	async importPortfolio(pf) {
		const copy: Portfolio = {
			...pf,
			id: uid(),
			updated_at: now(),
			positions: pf.positions ?? [],
		};
		saveAll([...loadAll(), copy]);
		return copy;
	},
};

/** All local portfolios — used by the migration prompt on first login. */
export function readLocalPortfolios(): Portfolio[] {
	return loadAll();
}
export function clearLocalPortfolios() {
	saveAll([]);
}

/* ── Server (authenticated) ────────────────────────────────────────────── */

async function unwrap<T>(
	p: Promise<{ data?: T; error?: unknown }>,
): Promise<T> {
	const { data, error } = await p;
	if (error) throw error;
	return data as T;
}

export const serverRepository: PortfolioRepository = {
	kind: "server",
	async list() {
		const s = signalWithTimeout(undefined);
		try {
			return (await unwrap(
				api.GET("/api/portfolios", { signal: s }),
			)) as PortfolioSummary[];
		} finally {
			s.cleanup();
		}
	},
	async get(id) {
		const s = signalWithTimeout(undefined);
		try {
			return (await unwrap(
				api.GET("/api/portfolios/{portfolio_id}", {
					params: { path: { portfolio_id: id } },
					signal: s,
				}),
			)) as Portfolio;
		} finally {
			s.cleanup();
		}
	},
	async create(name) {
		return (await unwrap(
			api.POST("/api/portfolios", { params: { query: { name } } }),
		)) as Portfolio;
	},
	async rename(id, name) {
		const pf = await this.get(id);
		if (!pf) return;
		await unwrap(
			api.PUT("/api/portfolios/{portfolio_id}", {
				params: { path: { portfolio_id: id } },
				body: { ...pf, name } as never,
			}),
		);
	},
	async remove(id) {
		await unwrap(
			api.DELETE("/api/portfolios/{portfolio_id}", {
				params: { path: { portfolio_id: id } },
			}),
		);
	},
	async putPositions(id, positions) {
		const pf = await this.get(id);
		if (!pf) throw new Error("Portfolio not found");
		return (await unwrap(
			api.PUT("/api/portfolios/{portfolio_id}", {
				params: { path: { portfolio_id: id } },
				body: { ...pf, positions } as never,
			}),
		)) as Portfolio;
	},
	async importPortfolio(pf) {
		const created = await this.create(pf.name);
		return this.putPositions(created.id, pf.positions ?? []);
	},
};
