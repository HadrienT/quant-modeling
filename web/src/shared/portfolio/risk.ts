import type { Instrument, PositionMark } from "@/shared/api";
import { underlyingOf } from "./ledger";

/**
 * Portfolio risk aggregation — blueprint WP 09 §4, pitfalls.
 * Summing greeks across DIFFERENT underlyings is meaningless, so greeks are
 * aggregated PER underlying. Values are in the portfolio's base currency;
 * greeks are position greeks (quantity × unit greek, instrument currency),
 * so an equity line contributes its share count to delta.
 */

export type RiskAggregate = {
	valuedCount: number;
	openCount: number;
	byUnderlying: {
		underlying: string;
		marketValue: number;
		unrealised: number;
		delta: number;
		gamma: number;
		vega: number;
		theta: number;
		rho: number;
	}[];
};

export function aggregateRisk(
	positions: PositionMark[],
	instruments: Instrument[],
): RiskAggregate {
	const byId = new Map(instruments.map((i) => [i.id, i]));
	const groups = new Map<string, RiskAggregate["byUnderlying"][number]>();
	let valued = 0;
	let open = 0;

	for (const p of positions) {
		if (p.quantity === 0) continue;
		open++;
		if (p.market_value_base == null) continue;
		valued++;
		const key = underlyingOf(byId.get(p.instrument_id));
		const g = p.greeks ?? {};
		const q = p.quantity;
		const acc = groups.get(key) ?? {
			underlying: key,
			marketValue: 0,
			unrealised: 0,
			delta: 0,
			gamma: 0,
			vega: 0,
			theta: 0,
			rho: 0,
		};
		acc.marketValue += p.market_value_base;
		acc.unrealised += p.unrealised_base ?? 0;
		acc.delta += q * (g.delta ?? 0);
		acc.gamma += q * (g.gamma ?? 0);
		acc.vega += q * (g.vega ?? 0);
		acc.theta += q * (g.theta ?? 0);
		acc.rho += q * (g.rho ?? 0);
		groups.set(key, acc);
	}

	return {
		valuedCount: valued,
		openCount: open,
		byUnderlying: [...groups.values()].sort(
			(a, b) => Math.abs(b.marketValue) - Math.abs(a.marketValue),
		),
	};
}
