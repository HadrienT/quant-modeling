import type { Position } from "@/shared/api";

/**
 * Portfolio risk aggregation — blueprint WP 09 §4, pitfalls.
 * Summing greeks across DIFFERENT underlyings is meaningless, so greeks are
 * aggregated PER underlying; a global total is only offered with that caveat.
 * The oldest pricing timestamp is surfaced so a stale, incoherent book shows it.
 */

export type RiskAggregate = {
	totalNpv: number;
	totalPnl: number;
	pricedCount: number;
	totalCount: number;
	oldestPricedAt: string | null;
	byUnderlying: {
		underlying: string;
		npv: number;
		pnl: number;
		delta: number;
		gamma: number;
		vega: number;
		theta: number;
		rho: number;
		mcStdError: number;
	}[];
};

function underlyingOf(p: Position): string {
	const params = p.parameters as Record<string, unknown>;
	return (
		(params.ticker as string) ??
		(params.underlying as string) ??
		p.label.split(/\s+/)[0] ??
		"—"
	);
}

export function aggregateRisk(positions: Position[]): RiskAggregate {
	const groups = new Map<string, RiskAggregate["byUnderlying"][number]>();
	let totalNpv = 0;
	let totalPnl = 0;
	let priced = 0;
	let oldest: string | null = null;

	for (const p of positions) {
		const r = p.result;
		if (!r) continue;
		priced++;
		const dir = p.direction === "short" ? -1 : 1;
		const q = dir * p.quantity;
		const npv = r.npv;
		const pnl = r.npv - p.entry_price * p.quantity * dir;
		totalNpv += npv;
		totalPnl += pnl;
		if (r.priced_at && (!oldest || r.priced_at < oldest)) oldest = r.priced_at;

		const key = underlyingOf(p);
		const g = r.greeks ?? {};
		const acc = groups.get(key) ?? {
			underlying: key,
			npv: 0,
			pnl: 0,
			delta: 0,
			gamma: 0,
			vega: 0,
			theta: 0,
			rho: 0,
			mcStdError: 0,
		};
		acc.npv += npv;
		acc.pnl += pnl;
		acc.delta += q * (g.delta ?? 0);
		acc.gamma += q * (g.gamma ?? 0);
		acc.vega += q * (g.vega ?? 0);
		acc.theta += q * (g.theta ?? 0);
		acc.rho += q * (g.rho ?? 0);
		acc.mcStdError = Math.hypot(acc.mcStdError, r.mc_std_error ?? 0);
		groups.set(key, acc);
	}

	return {
		totalNpv,
		totalPnl,
		pricedCount: priced,
		totalCount: positions.length,
		oldestPricedAt: oldest,
		byUnderlying: [...groups.values()].sort((a, b) => b.npv - a.npv),
	};
}
