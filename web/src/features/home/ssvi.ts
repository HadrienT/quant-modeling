import { type SurfaceGrid, makeGrid } from "@/shared/viz";

/**
 * The home page's surface: SSVI with a power-law curvature (Gatheral &
 * Jacquier, "Arbitrage-free SVI volatility surfaces", Quantitative Finance
 * 14(1), 2014, §4). With θ_t increasing and η(1 + |ρ|) ≤ 2, γ ∈ (0, ½] it is
 * free of static arbitrage (their Remark 4.4); `ssvi.test.ts` checks both the
 * calendar and the butterfly (Durrleman) conditions numerically on the grid
 * shown. Rates and dividends are zero, so the forward is the spot.
 */
export const SSVI = {
	spot: 100,
	atmVol: 0.2,
	rho: -0.7,
	eta: 1,
	gamma: 0.5,
} as const;

const STRIKES = Array.from({ length: 29 }, (_, i) => 60 + i * 3);
const MATURITIES = [0.08, 0.17, 0.25, 0.5, 0.75, 1, 1.5, 2];

/** ATM total variance θ_t: flat 20 % ATM vol, so increasing in t. */
export const theta = (t: number) => SSVI.atmVol * SSVI.atmVol * t;

const phi = (th: number) =>
	SSVI.eta / (th ** SSVI.gamma * (1 + th) ** (1 - SSVI.gamma));

/** Total implied variance w(k, θ) at log-moneyness k. */
export function totalVariance(k: number, th: number): number {
	const p = phi(th);
	const { rho } = SSVI;
	return (
		(th / 2) * (1 + rho * p * k + Math.sqrt((p * k + rho) ** 2 + 1 - rho * rho))
	);
}

/** An exact surface: no outlier to clip, so the display range is its range. */
export function ssviGrid(): SurfaceGrid {
	const grid = makeGrid(
		STRIKES,
		MATURITIES,
		MATURITIES.map((t) =>
			STRIKES.map((K) =>
				Math.sqrt(totalVariance(Math.log(K / SSVI.spot), theta(t)) / t),
			),
		),
		{
			x: { label: "Strike", format: (v) => v.toFixed(0) },
			y: {
				label: "Maturity",
				format: (v) => (v < 1 ? `${Math.round(v * 12)}m` : `${v}y`),
			},
			z: { label: "Implied vol", format: (v) => `${(v * 100).toFixed(1)}%` },
		},
	);
	return { ...grid, displayQuantiles: [0, 1] };
}

export const SSVI_STRIKES = STRIKES;
export const SSVI_MATURITIES = MATURITIES;
