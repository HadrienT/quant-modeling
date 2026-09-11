/**
 * Black-Scholes in the front end — blueprint WP 10 pitfalls.
 *
 * This duplicates formulae that also live in the C++ engines, on purpose: the
 * strategy visualiser needs an instant "value at t" curve as strikes are
 * dragged. blackScholes.test.ts pins it against published reference values AND
 * against put-call parity so the two implementations cannot drift silently.
 *
 * Continuous dividend yield q. Rates and vol are decimals (0.04, 0.2).
 */

/** Standard normal CDF Φ(x). Abramowitz & Stegun 7.1.26 (|error| < 7.5e-8). */
export function normCdf(x: number): number {
	// Abramowitz & Stegun 7.1.26
	const t = 1 / (1 + 0.2316419 * Math.abs(x));
	const d =
		0.31938153 * t -
		0.356563782 * t * t +
		1.781477937 * t ** 3 -
		1.821255978 * t ** 4 +
		1.330274429 * t ** 5;
	const p = 1 - (Math.exp((-x * x) / 2) / Math.sqrt(2 * Math.PI)) * d;
	return x >= 0 ? p : 1 - p;
}

function npdf(x: number): number {
	return Math.exp((-x * x) / 2) / Math.sqrt(2 * Math.PI);
}

export type BsInputs = {
	spot: number;
	strike: number;
	rate: number;
	dividend: number;
	vol: number;
	maturity: number;
	isCall: boolean;
};

export type BsResult = {
	price: number;
	delta: number;
	gamma: number;
	vega: number; // per 1.00 change in vol (not per vol point)
	theta: number; // per year
	rho: number;
};

export function blackScholes(i: BsInputs): BsResult {
	const {
		spot: S,
		strike: K,
		rate: r,
		dividend: q,
		vol: v,
		maturity: T,
		isCall,
	} = i;

	if (T <= 0 || v <= 0) {
		const intrinsic = Math.max(0, isCall ? S - K : K - S);
		return {
			price: intrinsic,
			delta: isCall ? (S > K ? 1 : 0) : S < K ? -1 : 0,
			gamma: 0,
			vega: 0,
			theta: 0,
			rho: 0,
		};
	}

	const sqrtT = Math.sqrt(T);
	const d1 = (Math.log(S / K) + (r - q + (v * v) / 2) * T) / (v * sqrtT);
	const d2 = d1 - v * sqrtT;
	const dfq = Math.exp(-q * T);
	const dfr = Math.exp(-r * T);
	const Nd1 = normCdf(d1);
	const Nd2 = normCdf(d2);

	if (isCall) {
		return {
			price: S * dfq * Nd1 - K * dfr * Nd2,
			delta: dfq * Nd1,
			gamma: (dfq * npdf(d1)) / (S * v * sqrtT),
			vega: S * dfq * npdf(d1) * sqrtT,
			theta:
				-(S * dfq * npdf(d1) * v) / (2 * sqrtT) -
				r * K * dfr * Nd2 +
				q * S * dfq * Nd1,
			rho: K * T * dfr * Nd2,
		};
	}
	return {
		price: K * dfr * normCdf(-d2) - S * dfq * normCdf(-d1),
		delta: -dfq * normCdf(-d1),
		gamma: (dfq * npdf(d1)) / (S * v * sqrtT),
		vega: S * dfq * npdf(d1) * sqrtT,
		theta:
			-(S * dfq * npdf(d1) * v) / (2 * sqrtT) +
			r * K * dfr * normCdf(-d2) -
			q * S * dfq * normCdf(-d1),
		rho: -K * T * dfr * normCdf(-d2),
	};
}
