import type { Leg, Metrics } from "./types";

/* ── single-leg payoff at expiry ───────────────────── */

export function legPayoff(spot: number, leg: Leg): number {
	const sign = leg.side === "long" ? 1 : -1;
	const q = leg.quantity;
	switch (leg.type) {
		case "call":
			return sign * q * (Math.max(spot - leg.strike, 0) - leg.premium);
		case "put":
			return sign * q * (Math.max(leg.strike - spot, 0) - leg.premium);
		case "stock":
		case "forward":
			return sign * q * (spot - leg.strike);
		case "digital-call":
			return sign * q * ((spot >= leg.strike ? leg.cashAmount : 0) - leg.premium);
		case "digital-put":
			return sign * q * ((spot <= leg.strike ? leg.cashAmount : 0) - leg.premium);
		default:
			return 0;
	}
}

/* ── portfolio aggregate ───────────────────────────── */

export function portfolioPayoff(spot: number, legs: Leg[]): number {
	return legs.reduce((sum, leg) => sum + legPayoff(spot, leg), 0);
}

/* ── uniform spot grid ─────────────────────────────── */

export function buildGrid(min: number, max: number, steps = 200): number[] {
	const out: number[] = [];
	const dx = (max - min) / steps;
	for (let i = 0; i <= steps; i += 1) out.push(min + dx * i);
	return out;
}

/* ── breakeven zero-crossings (linear interp) ──────── */

export function findBreakevens(legs: Leg[], grid: number[]): number[] {
	const be: number[] = [];
	for (let i = 0; i < grid.length - 1; i += 1) {
		const y0 = portfolioPayoff(grid[i], legs);
		const y1 = portfolioPayoff(grid[i + 1], legs);
		if ((y0 <= 0 && y1 > 0) || (y0 >= 0 && y1 < 0)) {
			const t = y0 / (y0 - y1);
			be.push(grid[i] + t * (grid[i + 1] - grid[i]));
		}
	}
	return be;
}

/* ── key metrics ───────────────────────────────────── */

export function computeMetrics(legs: Leg[], grid: number[]): Metrics {
	if (legs.length === 0) return { maxProfit: 0, maxLoss: 0, breakevens: [], netPremium: 0 };

	const payoffs = grid.map((s) => portfolioPayoff(s, legs));
	const maxProfit = Math.max(...payoffs);
	const maxLoss = Math.min(...payoffs);
	const breakevens = findBreakevens(legs, grid);

	const netPremium = legs.reduce((sum, leg) => {
		const sign = leg.side === "long" ? 1 : -1;
		return sum + sign * leg.quantity * leg.premium;
	}, 0);

	return { maxProfit, maxLoss, breakevens, netPremium };
}
