import { type BsInputs, blackScholes } from "./blackScholes";

/**
 * Multi-leg strategy payoff engine — blueprint WP 10 §1. Pure, no React,
 * reused by the pricing workbench (WP 07) and the strategy page.
 */

export type LegKind = "call" | "put" | "underlying" | "forward";

export type Leg = {
	id: string;
	kind: LegKind;
	direction: "long" | "short";
	quantity: number;
	strike: number; // ignored for underlying
	maturity: number; // years; may differ between legs (calendar spread)
	premium: number; // paid (long) / received (short), per unit
};

export type MarketInputs = {
	spot: number;
	rate: number;
	dividend: number;
	vol: number;
};

const EPS = 1e-9;

function legPayoffAtExpiry(leg: Leg, S: number): number {
	const sign = leg.direction === "long" ? 1 : -1;
	let intrinsic: number;
	switch (leg.kind) {
		case "call":
			intrinsic = Math.max(0, S - leg.strike);
			break;
		case "put":
			intrinsic = Math.max(0, leg.strike - S);
			break;
		case "underlying":
			intrinsic = S;
			break;
		case "forward":
			intrinsic = S - leg.strike;
			break;
	}
	return sign * leg.quantity * (intrinsic - leg.premium);
}

function legValueAtT(
	leg: Leg,
	S: number,
	mkt: MarketInputs,
	tElapsed: number,
): number {
	const sign = leg.direction === "long" ? 1 : -1;
	const tau = Math.max(0, leg.maturity - tElapsed);
	if (leg.kind === "underlying") return sign * leg.quantity * (S - leg.premium);
	if (leg.kind === "forward") {
		const fwd = S * Math.exp((mkt.rate - mkt.dividend) * tau);
		return (
			sign *
			leg.quantity *
			((fwd - leg.strike) * Math.exp(-mkt.rate * tau) - leg.premium)
		);
	}
	const bs: BsInputs = {
		spot: S,
		strike: leg.strike,
		rate: mkt.rate,
		dividend: mkt.dividend,
		vol: mkt.vol,
		maturity: tau,
		isCall: leg.kind === "call",
	};
	return sign * leg.quantity * (blackScholes(bs).price - leg.premium);
}

export type PayoffResult = {
	spot: number[];
	atMaturity: number[];
	atT: number[];
	legs: { label: string; values: number[] }[];
	greeks: { name: string; spot: number[]; values: number[] }[];
	breakevens: number[];
	maxGain: number | "unbounded";
	maxLoss: number | "unbounded";
	/**
	 * Net premium cash flow at inception, from the buyer's point of view.
	 * Negative = you pay net (a debit), positive = you receive net (a credit).
	 */
	netPremium: number;
};

export function evaluateStrategy(
	legs: Leg[],
	mkt: MarketInputs,
	{
		tElapsedFraction = 0.5,
		spotRange,
		points = 121,
	}: {
		tElapsedFraction?: number;
		spotRange?: [number, number];
		points?: number;
	} = {},
): PayoffResult {
	const maxMat = Math.max(...legs.map((l) => l.maturity), EPS);
	const [lo, hi] = spotRange ?? [mkt.spot * 0.6, mkt.spot * 1.4];
	const spot = Array.from(
		{ length: points },
		(_, i) => lo + ((hi - lo) * i) / (points - 1),
	);

	const atMaturity = spot.map((S) =>
		legs.reduce((a, l) => a + legPayoffAtExpiry(l, S), 0),
	);
	const tElapsed = maxMat * tElapsedFraction;
	const atT = spot.map((S) =>
		legs.reduce((a, l) => a + legValueAtT(l, S, mkt, tElapsed), 0),
	);

	const legSeries = legs.map((l) => ({
		label: `${l.direction === "long" ? "+" : "−"}${l.quantity} ${l.kind}${
			l.kind === "underlying" ? "" : ` ${l.strike}`
		}`,
		values: spot.map((S) => legPayoffAtExpiry(l, S)),
	}));

	// aggregate greeks vs spot (at t elapsed)
	const greekNames = ["Delta", "Gamma", "Vega", "Theta", "Rho"] as const;
	const greeks = greekNames.map((name) => ({
		name,
		spot,
		values: spot.map((S) =>
			legs.reduce((acc, l) => {
				if (l.kind === "underlying")
					return (
						acc +
						(name === "Delta"
							? (l.direction === "long" ? 1 : -1) * l.quantity
							: 0)
					);
				const tau = Math.max(EPS, l.maturity - tElapsed);
				const g = blackScholes({
					spot: S,
					strike: l.kind === "forward" ? l.strike : l.strike,
					rate: mkt.rate,
					dividend: mkt.dividend,
					vol: mkt.vol,
					maturity: tau,
					isCall: l.kind !== "put",
				});
				const sign = l.direction === "long" ? 1 : -1;
				const q = sign * l.quantity;
				switch (name) {
					case "Delta":
						return acc + q * g.delta;
					case "Gamma":
						return acc + q * g.gamma;
					case "Vega":
						return acc + q * g.vega;
					case "Theta":
						return acc + q * g.theta;
					case "Rho":
						return acc + q * g.rho;
				}
			}, 0),
		),
	}));

	// breakevens: numeric roots of atMaturity, refined by bisection (not grid-snapped)
	const breakevens: number[] = [];
	const f = (S: number) =>
		legs.reduce((a, l) => a + legPayoffAtExpiry(l, S), 0);
	for (let i = 1; i < spot.length; i++) {
		const a = spot[i - 1]!;
		const b = spot[i]!;
		if (f(a) === 0) breakevens.push(a);
		else if (f(a) * f(b) < 0) breakevens.push(bisect(f, a, b));
	}

	// bounded / unbounded
	const slopeLeft = f(lo) - f(lo - (hi - lo) * 0.01);
	const slopeRight = f(hi + (hi - lo) * 0.01) - f(hi);
	const values = atMaturity;
	const maxGain =
		slopeRight > EPS || slopeLeft < -EPS ? "unbounded" : Math.max(...values);
	const maxLoss =
		slopeRight < -EPS || slopeLeft > EPS ? "unbounded" : Math.min(...values);

	// Option premium cash flow at inception, from the buyer's side: premium paid
	// on long legs is an outflow (negative), received on short legs an inflow
	// (positive). Underlying legs are a capital outlay, not a premium, so they
	// are excluded here (they still enter the payoff and P&L).
	const netPremium = legs.reduce(
		(a, l) =>
			l.kind === "underlying"
				? a
				: a + (l.direction === "long" ? -1 : 1) * l.quantity * l.premium,
		0,
	);

	return {
		spot,
		atMaturity,
		atT,
		legs: legSeries,
		greeks,
		breakevens: dedupe(breakevens),
		maxGain,
		maxLoss,
		netPremium,
	};
}

function bisect(f: (x: number) => number, lo: number, hi: number): number {
	let a = lo;
	let b = hi;
	for (let i = 0; i < 60; i++) {
		const m = (a + b) / 2;
		if (f(a) * f(m) <= 0) b = m;
		else a = m;
	}
	return (a + b) / 2;
}

function dedupe(xs: number[]): number[] {
	const out: number[] = [];
	for (const x of xs.sort((p, q) => p - q)) {
		if (!out.length || Math.abs(out[out.length - 1]! - x) > 1e-4) out.push(x);
	}
	return out;
}

/** Named presets, parameterised from the current spot (WP 10 §2). */
export function preset(
	name: string,
	spot: number,
	maturity = 0.25,
): Leg[] | null {
	const id = () => Math.random().toString(36).slice(2, 8);
	const mk = (
		kind: LegKind,
		direction: "long" | "short",
		strike: number,
		premium = 0,
		quantity = 1,
	): Leg => ({
		id: id(),
		kind,
		direction,
		quantity,
		strike,
		maturity,
		premium,
	});
	const k = (pct: number) => Math.round(spot * pct);
	switch (name) {
		case "straddle":
			return [
				mk("call", "long", k(1), spot * 0.04),
				mk("put", "long", k(1), spot * 0.04),
			];
		case "strangle":
			return [
				mk("call", "long", k(1.05), spot * 0.02),
				mk("put", "long", k(0.95), spot * 0.02),
			];
		case "call-spread":
			return [
				mk("call", "long", k(1), spot * 0.04),
				mk("call", "short", k(1.1), spot * 0.015),
			];
		case "put-spread":
			return [
				mk("put", "long", k(1), spot * 0.04),
				mk("put", "short", k(0.9), spot * 0.015),
			];
		case "butterfly":
			return [
				mk("call", "long", k(0.95), spot * 0.06),
				mk("call", "short", k(1), spot * 0.035, 2),
				mk("call", "long", k(1.05), spot * 0.018),
			];
		case "condor":
			return [
				mk("call", "long", k(0.9), spot * 0.11),
				mk("call", "short", k(0.97), spot * 0.05),
				mk("call", "short", k(1.03), spot * 0.02),
				mk("call", "long", k(1.1), spot * 0.008),
			];
		case "collar":
			return [
				mk("underlying", "long", 0, spot),
				mk("put", "long", k(0.95), spot * 0.02),
				mk("call", "short", k(1.05), spot * 0.02),
			];
		case "risk-reversal":
			return [
				mk("call", "long", k(1.05), spot * 0.02),
				mk("put", "short", k(0.95), spot * 0.02),
			];
		case "covered-call":
			return [
				mk("underlying", "long", 0, spot),
				mk("call", "short", k(1.05), spot * 0.02),
			];
		case "protective-put":
			return [
				mk("underlying", "long", 0, spot),
				mk("put", "long", k(0.95), spot * 0.025),
			];
		case "calendar-spread":
			return [
				{ ...mk("call", "short", k(1), spot * 0.025), maturity: maturity / 2 },
				{ ...mk("call", "long", k(1), spot * 0.04), maturity },
			];
		default:
			return null;
	}
}
