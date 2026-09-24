import { api, migratePortfolio, signalWithTimeout } from "@/shared/api";
import type { Portfolio, PortfolioSummary } from "@/shared/api";
import { netQuantities } from "./ledger";

/** What a trade changes: the ledger (and the currency it is reported in). */
export type Ledger = Pick<
	Portfolio,
	"instruments" | "transactions" | "base_currency"
>;

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
	/** Replace the ledger; the positions are derived from it. */
	putLedger(id: string, ledger: Ledger): Promise<Portfolio>;
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
	const open = [...netQuantities(pf.transactions ?? []).values()].filter(
		(q) => q !== 0,
	).length;
	return {
		id: pf.id,
		name: pf.name,
		created_at: pf.created_at ?? "",
		updated_at: pf.updated_at ?? "",
		n_positions: open + (pf.positions?.length ?? 0),
		// the value needs market data: the valuation endpoints give it
		total_value: 0,
	};
}

/** A portfolio from before the ledger, rewritten as one (server-side rules). */
async function upgraded(pf: Portfolio): Promise<Portfolio> {
	if ((pf.version ?? 1) >= 2) return pf;
	try {
		return await migratePortfolio(pf);
	} catch {
		return pf; // offline: shown as is, migrated on the next visit
	}
}

export const localRepository: PortfolioRepository = {
	kind: "local",
	async list() {
		return loadAll().map(summarise);
	},
	async get(id) {
		const all = loadAll();
		const i = all.findIndex((p) => p.id === id);
		if (i < 0) return null;
		const pf = await upgraded(all[i]!);
		if (pf !== all[i]) {
			all[i] = pf;
			saveAll(all);
		}
		return pf;
	},
	async create(name) {
		const pf: Portfolio = {
			id: uid(),
			name,
			owner: "",
			created_at: now(),
			updated_at: now(),
			version: 2,
			base_currency: "EUR",
			instruments: [],
			transactions: [],
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
	async putLedger(id, ledger) {
		const all = loadAll();
		const i = all.findIndex((p) => p.id === id);
		if (i < 0) throw new Error("Portfolio not found");
		const pf: Portfolio = {
			...all[i]!,
			...ledger,
			version: 2,
			updated_at: now(),
		};
		all[i] = pf;
		saveAll(all);
		return pf;
	},
	async importPortfolio(pf) {
		const copy: Portfolio = {
			...(await upgraded(pf)),
			id: uid(),
			updated_at: now(),
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
	async putLedger(id, ledger) {
		const pf = await this.get(id);
		if (!pf) throw new Error("Portfolio not found");
		return (await unwrap(
			api.PUT("/api/portfolios/{portfolio_id}", {
				params: { path: { portfolio_id: id } },
				body: { ...pf, ...ledger, version: 2 } as never,
			}),
		)) as Portfolio;
	},
	async importPortfolio(pf) {
		const created = await this.create(pf.name);
		const ledger = await upgraded(pf);
		return this.putLedger(created.id, {
			instruments: ledger.instruments ?? [],
			transactions: ledger.transactions ?? [],
			base_currency: ledger.base_currency ?? "EUR",
		});
	},
};
