/**
 * Pedagogical content shown by the ⓘ popover next to a product selector
 * (see ../../components/ProductInfo.tsx).
 *
 * Keyed by the same string values used for `instrument` / `exoticProduct` /
 * `structuredProduct` / `volProduct` / `fxProduct` / `commodityProduct` in
 * ./types.ts, so a doc "just appears" next to the matching selector the
 * moment its entry is added here — no wiring changes needed elsewhere.
 *
 * A key with no entry simply renders no icon (see ProductInfo's `doc?`
 * prop) — that's how partial rollout works: fill these in gradually,
 * un-vetted products stay silent instead of showing a placeholder.
 *
 * Content here is describing THIS codebase's actual engines, not pricing
 * theory in the abstract — every "How it's priced here" bullet should be
 * checked against the engine source it describes whenever that engine
 * changes, the same way a docstring can drift from its implementation.
 */

export type PricingMethodEntry = {
	/** Engine label as shown in the "Pricing engine" dropdown. */
	engine: string;
	/** May contain inline math as `$...$`. */
	detail: string;
};

export type ProductDoc = {
	title: string;
	/** One or two sentences: what the product is. */
	summary: string;
	/** One or more KaTeX block formulas (rendered in order). */
	payoff: string | string[];
	/** Model / pricing assumptions. May contain inline math as `$...$`. */
	assumptions: string[];
	/** One entry per selectable engine for this product. */
	pricingMethods: PricingMethodEntry[];
	/** Limitations, conventions, free correctness checks, interview-relevant facts. */
	notes: string[];
};

export const PRODUCT_DOCS: Partial<Record<string, ProductDoc>> = {
	/* ── Vanilla European option (instrument = "option") ────────────── */
	option: {
		title: "Vanilla European Option",
		summary:
			"The right, but not the obligation, to buy (call) or sell (put) the underlying at a fixed strike $K$ on a single maturity date $T$. No path dependency — only the terminal spot $S_T$ enters the payoff.",
		payoff:
			"\\text{Payoff} = \\max(S_T - K,\\ 0)\\ \\text{(call)} \\qquad \\max(K - S_T,\\ 0)\\ \\text{(put)}",
		assumptions: [
			"The underlying follows geometric Brownian motion under the risk-neutral measure: $dS_t = (r-q)S_t\\,dt + \\sigma S_t\\,dW_t$, with constant risk-free rate $r$, dividend yield $q$ and volatility $\\sigma$ (flat — no smile, no term structure).",
			"European exercise only, at $T$. Selecting a tree or PDE engine below still prices this European payoff; American exercise is a separate instrument in this codebase's registry, not a flag on this one.",
			"No transaction costs, no bid-ask spread, continuous trading, no borrow constraints.",
		],
		pricingMethods: [
			{
				engine: "Analytic",
				detail:
					"Closed-form Black-Scholes-Merton formula. Exact under the model's own assumptions, and the fastest engine here — a good reference to sanity-check the others against.",
			},
			{
				engine: "Monte Carlo",
				detail:
					"Since the payoff depends only on $S_T$, the terminal spot is drawn directly from its exact log-normal distribution — no path is stepped through. Antithetic variates are always applied (every Gaussian draw $Z$ is paired with $-Z$) to cut variance; you only control the path count, seed and MC epsilon (a payoff-smoothing bump that a plain vanilla payoff doesn't need).",
			},
			{
				engine: "Binomial / Trinomial Tree",
				detail:
					"Discretizes $[0,T]$ into the chosen number of steps and backward-induces the price on a recombining tree. Converges to the analytic price as the step count grows; mainly useful here as the building block that also supports American exercise elsewhere in the codebase.",
			},
			{
				engine: "PDE (Crank-Nicolson)",
				detail:
					"Solves the Black-Scholes PDE $\\frac{\\partial V}{\\partial t} + \\tfrac12\\sigma^2S^2\\frac{\\partial^2V}{\\partial S^2} + (r-q)S\\frac{\\partial V}{\\partial S} - rV = 0$ backward in time on a finite-difference grid sized by the space/time step counts you set.",
			},
		],
		notes: [
			"Greeks ($\\Delta,\\Gamma,\\text{vega},\\theta,\\rho$) are computed the same way for every engine: central finite differences on the pricing function itself — bump an input, reprice, divide by twice the bump. Simple and engine-agnostic, but costs an extra repricing per Greek and isn't exact to machine precision the way adjoint differentiation (AAD) would be — this codebase doesn't implement AAD yet.",
			"Put-call parity, $C - P = S_0e^{-qT} - Ke^{-rT}$, is a free correctness check: price both and the identity should hold to within Monte Carlo noise.",
		],
	},

	/* ── Barrier option (exoticProduct = "barrier") ─────────────────── */
	barrier: {
		title: "Barrier Option",
		summary:
			'A vanilla call or put that only comes alive if the spot touches a barrier level $H$ before maturity ("knock-in"), or that dies if it does ("knock-out"). Four flavours: up/down × in/out.',
		payoff:
			"\\text{Payoff} = \\underbrace{\\max(\\phi(S_T-K),\\,0)}_{\\text{vanilla payoff},\\ \\phi=\\pm1}\\times\\mathbb{1}\\{\\text{barrier condition met}\\}\\ +\\ R\\times\\mathbb{1}\\{\\text{barrier condition not met}\\}",
		assumptions: [
			"Same Black-Scholes GBM dynamics as the vanilla option: constant $r$, $q$, $\\sigma$.",
			'For a knock-out, "condition met" means the barrier was never breached; for a knock-in, it means the barrier was breached — the rebate $R$ (default 0) applies to the opposite, "dead", case in each.',
			"Monitoring is discrete (weekly, i.e. 52 dates/year, by default), not truly continuous — the toggle below controls whether the gaps between those dates are corrected for or ignored.",
		],
		pricingMethods: [
			{
				engine: "Monte Carlo (only engine for this product)",
				detail:
					"Simulates $S_t$ at each monitoring date under GBM. With the Brownian-bridge toggle on, instead of only checking whether the simulated dates themselves cross $H$, the engine computes the exact probability that a Brownian bridge between two consecutive dates stays clear of $H$ and weights the payoff by that probability (conditional Monte Carlo) — this removes the discrete monitoring bias without needing to simulate a full daily path, and is what keeps the barrier Greeks estimable by finite differences without excessive noise.",
			},
		],
		notes: [
			"Greeks use common random numbers (CRN): every bumped repricing (for $\\Delta,\\Gamma,\\text{vega},\\theta,\\rho$) reuses the exact same random draws as the base path, so the finite-difference Greeks are far less noisy than if each bump redrew fresh paths.",
			"In + Out = Vanilla: an up-and-in and an up-and-out call with the same strike and barrier must sum to the plain vanilla call price. A gap beyond MC noise signals a bug — a useful free correctness test.",
			'The rebate, if any, is paid at expiry $T$ for a "dead" path in this implementation, not at the moment the barrier is hit — a simplifying convention; some real-world barrier notes pay the rebate at the hit date instead, which is worth more (it arrives earlier).',
			"Daily monitoring (252/year) is more accurate than the weekly default but costs roughly $9\\times$ more compute per path here (one base path plus eight bumped variants for the five Greeks) — weekly is kept as the default to stay well inside the API's response-time budget.",
		],
	},

	/* ── Autocallable note (structuredProduct = "autocall") ─────────── */
	autocall: {
		title: "Autocallable Note",
		summary:
			"A structured note on a single underlying that redeems early with a coupon if the spot is at or above a barrier on any observation date, and otherwise exposes the investor to downside below a knock-in barrier if it survives to maturity.",
		payoff: [
			"\\text{At each } T_i:\\quad S(T_i)\\ge B_{ac}S_0 \\;\\Longrightarrow\\; \\text{redeemed for } N\\,(1+c_i)",
			"\\text{If alive at } T_n:\\quad\\begin{cases} N\\,(1+c_n) & S(T_n)\\ge B_{cpn}S_0 \\\\ N & B_{ki}S_0\\le S(T_n) < B_{cpn}S_0 \\\\ N\\cdot S(T_n)/S_0 & S(T_n) < B_{ki}S_0 \\end{cases}",
		],
		assumptions: [
			"Single underlying, same Black-Scholes GBM dynamics as the vanilla option.",
			"All barriers ($B_{ac}$, $B_{cpn}$, $B_{ki}$) are expressed as a fraction of $S_0$ — e.g. $B_{ac}=1.0$ means the autocall trigger is at-the-money.",
			'"Memory" coupons (on by default): a coupon missed at an earlier date because the coupon barrier wasn\'t met is paid later if a subsequent date clears it, rather than being permanently lost.',
		],
		pricingMethods: [
			{
				engine: "Monte Carlo (only engine for this product)",
				detail:
					"The spot is simulated exactly at each observation date via the closed-form GBM transition $S(T_i)=S(T_{i-1})\\exp\\!\\big((r-q-\\tfrac12\\sigma^2)\\Delta t + \\sigma\\sqrt{\\Delta t}\\,Z_i\\big)$ — there's no need to step through a full daily path, since only the observation dates matter to the payoff. All barrier checks (autocall, coupon, knock-in) read the same simulated path, so they stay mutually consistent within each Monte Carlo draw.",
			},
		],
		notes: [
			'Unlike the barrier engine, the "continuous knock-in" flag here does not apply a Brownian-bridge correction — it only checks the knock-in barrier at every observation date instead of only the final one. Genuine continuous-time touches between observation dates aren\'t captured; read "continuous" as "checked more often", not as true continuous monitoring.',
			"An autocall is economically short a knock-in put struck at $B_{ki}S_0$ dressed up with a coupon — worth stating plainly, since the coupon can make the product look lower-risk than the downside it's actually carrying.",
		],
	},
};
