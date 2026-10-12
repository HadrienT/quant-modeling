import type { Instrument, PositionMark } from "@/shared/api";
import { underlyingOf } from "./ledger";

/**
 * Portfolio risk aggregation — blueprint WP 09 §4, pitfalls.
 * Summing greeks across DIFFERENT underlyings is meaningless, so greeks are
 * aggregated PER underlying. Values are in the portfolio's base currency;
 * greeks are position greeks (quantity × unit greek, instrument currency),
 * so an equity line contributes its share count to delta.
 */

/**
 * What a rate or FX trade moves by, for the position held (quantity × the
 * contract's), in the currency of its mark: per basis point of each swap
 * curve, per dollar of EUR/USD (the euros it is long of), per basis point of
 * normal volatility. null where the trade does not depend on it.
 */
export type RateSensitivities = {
	instrumentId: string;
	label: string;
	currency: string;
	pv01Usd: number | null;
	pv01Eur: number | null;
	fxDelta: number | null;
	normalVega: number | null;
};

export type RiskAggregate = {
	valuedCount: number;
	openCount: number;
	/** rate and FX trades, one row each: their risks do not add up to greeks */
	rates: RateSensitivities[];
	byUnderlying: {
		underlying: string;
		/** false for rate and FX trades, which have no equity greeks */
		greeks: boolean;
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
	const rates: RateSensitivities[] = [];
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
		const contract = p.kind === "rate_fx";
		if (contract) {
			const held = (v: number | null | undefined) => (v == null ? null : q * v);
			rates.push({
				instrumentId: p.instrument_id,
				label: p.label,
				currency: p.currency,
				pv01Usd: held(g.pv01_usd),
				pv01Eur: held(g.pv01_eur),
				fxDelta: held(g.fx_delta),
				normalVega: held(g.normal_vega),
			});
		}
		const acc = groups.get(key) ?? {
			underlying: key,
			greeks: !contract,
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
		rates,
		byUnderlying: [...groups.values()].sort(
			(a, b) => Math.abs(b.marketValue) - Math.abs(a.marketValue),
		),
	};
}
