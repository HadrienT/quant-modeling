/**
 * Local-storage backed portfolio management for anonymous (not logged-in) users.
 * Mirrors the API contract so the Portfolio page can swap seamlessly.
 */
import type {
	Portfolio,
	PortfolioSummary,
	Position,
} from "../api/client";

const LS_KEY = "qm_local_portfolios";

function uid(): string {
	return crypto.randomUUID?.() ?? Math.random().toString(36).slice(2, 14);
}

function loadAll(): Portfolio[] {
	try {
		const raw = localStorage.getItem(LS_KEY);
		return raw ? (JSON.parse(raw) as Portfolio[]) : [];
	} catch {
		return [];
	}
}

function saveAll(portfolios: Portfolio[]) {
	localStorage.setItem(LS_KEY, JSON.stringify(portfolios));
}

function now(): string {
	return new Date().toISOString();
}

// ── Public API (matches the shape of the remote API) ────────────────────────

export function localListPortfolios(): PortfolioSummary[] {
	return loadAll().map((pf) => ({
		id: pf.id,
		name: pf.name,
		created_at: pf.created_at,
		updated_at: pf.updated_at,
		n_positions: pf.positions.length,
		total_value: pf.positions.reduce((s, p) => s + (p.result?.npv ?? 0), 0),
	}));
}

export function localCreatePortfolio(name: string): Portfolio {
	const pf: Portfolio = {
		id: uid(),
		name,
		created_at: now(),
		updated_at: now(),
		positions: [],
	};
	const all = loadAll();
	all.push(pf);
	saveAll(all);
	return pf;
}

export function localGetPortfolio(id: string): Portfolio | null {
	return loadAll().find((p) => p.id === id) ?? null;
}

export function localDeletePortfolio(id: string): void {
	saveAll(loadAll().filter((p) => p.id !== id));
}

export function localAddPosition(portfolioId: string, pos: Position): Portfolio {
	const all = loadAll();
	const pf = all.find((p) => p.id === portfolioId);
	if (!pf) throw new Error("Portfolio not found");
	if (!pos.id) pos.id = uid();
	pf.positions.push(pos);
	pf.updated_at = now();
	saveAll(all);
	return { ...pf };
}

export function localUpdatePosition(portfolioId: string, pos: Position): Portfolio {
	const all = loadAll();
	const pf = all.find((p) => p.id === portfolioId);
	if (!pf) throw new Error("Portfolio not found");
	const idx = pf.positions.findIndex((p) => p.id === pos.id);
	if (idx === -1) throw new Error("Position not found");
	pf.positions[idx] = pos;
	pf.updated_at = now();
	saveAll(all);
	return { ...pf };
}

export function localRemovePosition(portfolioId: string, positionId: string): Portfolio {
	const all = loadAll();
	const pf = all.find((p) => p.id === portfolioId);
	if (!pf) throw new Error("Portfolio not found");
	pf.positions = pf.positions.filter((p) => p.id !== positionId);
	pf.updated_at = now();
	saveAll(all);
	return { ...pf };
}

/**
 * Save a priced portfolio back to localStorage (after remote pricing).
 */
export function localSavePortfolio(pf: Portfolio): void {
	const all = loadAll();
	const idx = all.findIndex((p) => p.id === pf.id);
	if (idx >= 0) {
		all[idx] = pf;
	} else {
		all.push(pf);
	}
	saveAll(all);
}
