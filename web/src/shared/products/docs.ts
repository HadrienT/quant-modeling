/**
 * Product doc cards — blueprint WP 99, extended with a sourced bibliography
 * (`references`) for the dedicated Products page. The prose describes THIS
 * repo's engines (src/engines/, src/instruments/), not pricing theory in the
 * abstract, and ages like a docstring: re-check each "How it's priced here"
 * bullet against the engine source whenever that engine changes.
 *
 * `references` is the exception — it points at the founding paper(s) and a
 * standard textbook chapter, which do not age. Keep 1–3 entries per product,
 * most canonical first. A `url` is added only when the DOI is certain; the
 * citation text is always complete enough to find the work without a link.
 *
 * The `option`, `barrier` and `autocall` entries are the recovered work
 * (blueprint WP 99); the rest are written from the request schemas and engine
 * layout and are worth a maintainer pass against src/ before being treated as
 * authoritative.
 */

export type PricingMethodEntry = { engine: string; detail: string };

export type Reference = {
	kind: "paper" | "book" | "note";
	/** Author(s) and year, e.g. "Black & Scholes (1973)". */
	label: string;
	/** Title, then venue with volume / pages, or edition / chapter for a book. */
	cite: string;
	/** Stable link — a DOI only where its correctness is certain. */
	url?: string;
};

export type ModelRequirement = {
	/** The minimum model class that reproduces this payoff's risk correctly. */
	minimum: string;
	/** Which specific feature of the payoff forces that floor — the "why", not just the "what". */
	rationale: string;
};

export type ProductDoc = {
	title: string;
	summary: string;
	/** one or more KaTeX block formulas */
	payoff: string | string[];
	assumptions: string[];
	pricingMethods: PricingMethodEntry[];
	notes: string[];
	/** Founding paper(s) and a standard textbook chapter — see file header. */
	references: Reference[];
	/**
	 * Which model is the minimum one that captures the *right* dynamics for
	 * this payoff, and why a simpler one gets it wrong — not what this repo
	 * happens to implement (see pricingMethods for that), but what a desk
	 * pricing this payoff for real would need underneath.
	 */
	modelDependency: ModelRequirement;
};

const bsDynamics =
	"The underlying follows geometric Brownian motion under the risk-neutral measure, $dS_t=(r-q)S_t\\,dt+\\sigma S_t\\,dW_t$, with constant $r$, $q$ and $\\sigma$ (flat vol — no smile, no term structure).";

/** Reused textbook references. */
const HULL: Reference = {
	kind: "book",
	label: "Hull (2021)",
	cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson.",
};
const GLASSERMAN: Reference = {
	kind: "book",
	label: "Glasserman (2004)",
	cite: "Monte Carlo Methods in Financial Engineering, Springer.",
};
const BLACK_SCHOLES_1973: Reference = {
	kind: "paper",
	label: "Black & Scholes (1973)",
	cite: "The Pricing of Options and Corporate Liabilities. Journal of Political Economy 81(3), 637–654.",
	url: "https://doi.org/10.1086/260062",
};
const MERTON_1973: Reference = {
	kind: "paper",
	label: "Merton (1973)",
	cite: "Theory of Rational Option Pricing. The Bell Journal of Economics and Management Science 4(1), 141–183.",
	url: "https://doi.org/10.2307/3003143",
};
const BLACK_1976: Reference = {
	kind: "paper",
	label: "Black (1976)",
	cite: "The Pricing of Commodity Contracts. Journal of Financial Economics 3(1–2), 167–179.",
	url: "https://doi.org/10.1016/0304-405X(76)90024-6",
};
const BOUZOUBAA: Reference = {
	kind: "book",
	label: "Bouzoubaa & Osseiran (2010)",
	cite: "Exotic Options and Hybrids: A Guide to Structuring, Pricing and Trading, Wiley.",
};

export const PRODUCT_DOCS: Partial<Record<string, ProductDoc>> = {
	option: {
		title: "Vanilla European option",
		summary:
			"The right, not the obligation, to buy (call) or sell (put) the underlying at a fixed strike $K$ on a single maturity $T$. Only the terminal spot $S_T$ enters the payoff — no path dependency.",
		payoff:
			"\\max(S_T-K,\\,0)\\ \\text{(call)} \\qquad \\max(K-S_T,\\,0)\\ \\text{(put)}",
		assumptions: [
			bsDynamics,
			"European exercise only. Selecting a tree or PDE engine still prices this European payoff; American exercise is a separate catalog entry, not a flag here.",
			"No transaction costs, continuous trading, no borrow constraints.",
		],
		pricingMethods: [
			{
				engine: "Analytic",
				detail:
					"Closed-form Black-Scholes-Merton. Exact under the model, fastest engine, and the reference the others are checked against.",
			},
			{
				engine: "Monte-Carlo",
				detail:
					"The payoff depends only on $S_T$, so the terminal spot is drawn directly from its exact log-normal law — no path is stepped. Antithetic variates are always on; you control path count, seed and the MC-epsilon payoff smoothing bump.",
			},
			{
				engine: "Binomial / trinomial tree",
				detail:
					"Backward induction on a recombining lattice over the chosen number of steps; converges to the analytic price. Mainly the building block reused for American exercise.",
			},
			{
				engine: "PDE (Crank-Nicolson)",
				detail:
					"Solves the Black-Scholes PDE backward in time on a finite-difference grid sized by the space/time step counts.",
			},
		],
		notes: [
			"Greeks are central finite differences on the pricing function for every engine — bump an input, reprice, divide by twice the bump. Engine-agnostic but costs an extra repricing per greek; no adjoint (AAD) yet.",
			"Put-call parity $C-P=S_0e^{-qT}-Ke^{-rT}$ is a free correctness check across engines.",
		],
		references: [
			BLACK_SCHOLES_1973,
			MERTON_1973,
			{
				...HULL,
				cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson — ch. 15–19.",
			},
		],

		modelDependency: {
			minimum: "Flat Black-Scholes",
			rationale:
				"The payoff reads $S_T$ at one date. A single constant $\\sigma$ can always be chosen to match the market-quoted implied vol at exactly that strike and maturity — there is only one point on the smile to get right, so there is no smile to get wrong.",
		},
	},

	american: {
		title: "American vanilla option",
		summary:
			"A call or put exercisable at any time up to $T$. The holder's early-exercise right adds value versus the European; for a non-dividend call it does not (never exercise early), for a put it generally does.",
		payoff:
			"\\sup_{\\tau\\le T}\\ \\mathbb{E}\\big[e^{-r\\tau}\\max(\\phi(S_\\tau-K),0)\\big],\\quad \\phi=\\pm1",
		assumptions: [
			bsDynamics,
			"Early exercise permitted at every lattice / grid time step (a discrete approximation to continuous exercise).",
		],
		pricingMethods: [
			{
				engine: "Binomial / trinomial tree",
				detail:
					"Backward induction: at each node the value is $\\max(\\text{continuation},\\ \\text{intrinsic})$. The exercise boundary emerges from where intrinsic first wins. Step count trades accuracy for latency.",
			},
			{
				engine: "PDE (Crank-Nicolson)",
				detail:
					"The free-boundary problem is handled by projecting the solution onto the payoff at each time step (an explicit obstacle constraint on the finite-difference grid).",
			},
		],
		notes: [
			"A non-dividend American call equals its European counterpart — a free sanity check (set $q=0$, compare to the vanilla analytic price).",
			"American value is monotone in every step count refinement; oscillation as steps grow signals a lattice parameterisation issue.",
		],
		references: [
			{
				kind: "paper",
				label: "Cox, Ross & Rubinstein (1979)",
				cite: "Option Pricing: A Simplified Approach. Journal of Financial Economics 7(3), 229–263.",
				url: "https://doi.org/10.1016/0304-405X(79)90015-1",
			},
			{
				kind: "paper",
				label: "Brennan & Schwartz (1977)",
				cite: "The Valuation of American Put Options. The Journal of Finance 32(2), 449–462.",
			},
			{
				kind: "paper",
				label: "Longstaff & Schwartz (2001)",
				cite: "Valuing American Options by Simulation: A Simple Least-Squares Approach. The Review of Financial Studies 14(1), 113–147. (The repo prices American exercise on lattices / the PDE grid, not by LSM regression.)",
				url: "https://doi.org/10.1093/rfs/14.1.113",
			},
			{
				...HULL,
				cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson — ch. 21 (trees), ch. 27 (finite differences).",
			},
		],

		modelDependency: {
			minimum: "Flat Black-Scholes, for one quoted vol",
			rationale:
				"Same single-date payoff character as the European option — early exercise moves the *timing* of the decision, not the smile it depends on. Pricing an American *book* consistently across strikes needs at least local vol: the exercise boundary at each spot level should react to that level's own implied vol, which one flat $\\sigma$ cannot supply away from the strike it was set to.",
		},
	},

	asian: {
		title: "Asian (average-price) option",
		summary:
			"The payoff uses an average of the underlying over a monitoring window instead of the terminal spot, which lowers the effective volatility and makes the option cheaper and harder to manipulate near expiry.",
		payoff:
			"\\max(\\bar S-K,\\,0),\\quad \\bar S=\\tfrac1n\\sum_i S_{t_i}\\ \\text{(arithmetic)}\\ \\text{or}\\ \\Big(\\prod_i S_{t_i}\\Big)^{1/n}\\ \\text{(geometric)}",
		assumptions: [
			bsDynamics,
			"Discrete, equally-spaced averaging dates over $[0,T]$.",
			"Arithmetic averaging has no closed form; geometric averaging does (the geometric mean of log-normals is log-normal).",
		],
		pricingMethods: [
			{
				engine: "Analytic (geometric only)",
				detail:
					"Closed form via an adjusted volatility and drift for the geometric average. Not available for the arithmetic payoff.",
			},
			{
				engine: "Monte-Carlo",
				detail:
					"Steps the path across the averaging dates and averages. The geometric-average price is used as a control variate for the arithmetic estimator — the two are highly correlated, so this cuts variance sharply for near-zero cost.",
			},
		],
		notes: [
			"Arithmetic ≥ geometric price always (AM–GM inequality on the averages) — a free ordering check.",
			"As the number of averaging dates → 1 the Asian collapses to a vanilla; as it → ∞ the price approaches the continuous-average limit.",
		],
		references: [
			{
				kind: "paper",
				label: "Kemna & Vorst (1990)",
				cite: "A Pricing Method for Options Based on Average Asset Values. Journal of Banking & Finance 14(1), 113–129. (Source of both the geometric closed form and the control-variate trick used here.)",
				url: "https://doi.org/10.1016/0378-4266(90)90039-5",
			},
			{
				kind: "paper",
				label: "Turnbull & Wakeman (1991)",
				cite: "A Quick Algorithm for Pricing European Average Options. Journal of Financial and Quantitative Analysis 26(3), 377–389.",
			},
			{
				...GLASSERMAN,
				cite: "Monte Carlo Methods in Financial Engineering, Springer — §4.1 (control variates).",
			},
		],

		modelDependency: {
			minimum: "Local volatility (Dupire)",
			rationale:
				"The average touches every monitoring date's own marginal distribution, not just the terminal one. A model that does not reproduce the market smile at each of those dates misprices the average whenever the smile has real curvature — flat vol is exact only in the degenerate case where the smile is flat too.",
		},
	},

	barrier: {
		title: "Barrier option",
		summary:
			'A vanilla call or put that only comes alive if the spot touches a barrier $H$ before maturity ("knock-in"), or that dies if it does ("knock-out"). Four flavours: up/down × in/out.',
		payoff:
			"\\max(\\phi(S_T-K),0)\\cdot\\mathbb{1}\\{\\text{barrier condition met}\\}\\ +\\ R\\cdot\\mathbb{1}\\{\\text{not met}\\}",
		assumptions: [
			"Same Black-Scholes GBM dynamics as the vanilla option.",
			"The rebate $R$ (default 0) is paid on the opposite, dead case.",
			"Monitoring is discrete (weekly, 52/year by default), not truly continuous — the toggle below decides whether the gaps are corrected.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"Simulates $S_t$ at each monitoring date. With the Brownian-bridge toggle on, the engine weights the payoff by the exact probability that a Brownian bridge between two consecutive dates stays clear of $H$ (conditional Monte-Carlo), removing the discrete-monitoring bias without a full daily path.",
			},
		],
		notes: [
			"Greeks use common random numbers: every bumped repricing reuses the base path's draws, so finite-difference greeks are far less noisy.",
			"In + Out = Vanilla for matching strike and barrier — a gap beyond MC noise is a bug.",
			"The rebate is paid at $T$ for a dead path here, not at the hit date (a simplifying convention worth slightly less).",
		],
		references: [
			{
				kind: "paper",
				label: "Merton (1973)",
				cite: "Theory of Rational Option Pricing. The Bell Journal of Economics and Management Science 4(1), 141–183. (§8 gives the first closed form for the continuously-monitored down-and-out call.)",
				url: "https://doi.org/10.2307/3003143",
			},
			{
				kind: "note",
				label: "Reiner & Rubinstein (1991)",
				cite: "Breaking Down the Barriers. Risk 4(8), 28–35. (The eight single-barrier closed forms.)",
			},
			{
				kind: "paper",
				label: "Broadie, Glasserman & Kou (1997)",
				cite: "A Continuity Correction for Discrete Barrier Options. Mathematical Finance 7(4), 325–349.",
				url: "https://doi.org/10.1111/1467-9965.00035",
			},
			{
				...GLASSERMAN,
				cite: "Monte Carlo Methods in Financial Engineering, Springer — §6.4 (Brownian-bridge conditional Monte-Carlo for barriers).",
			},
		],

		modelDependency: {
			minimum:
				"Local volatility (Dupire); stochastic-local vol for realistic hedging risk",
			rationale:
				"The hit probability is driven by the vol used out-of-the-money near the barrier, not the at-the-money vol a flat model would apply everywhere. Local vol matches today's smile at the barrier level; SLV is the desk standard because barrier risk also depends on how that smile *moves* as spot approaches the barrier (forward-skew risk), which local vol alone gets wrong in a well-documented direction.",
		},
	},

	digital: {
		title: "Digital (binary) option",
		summary:
			"Pays a fixed cash amount (or one unit of the asset) if the option finishes in the money, and nothing otherwise. A bet on direction, not magnitude.",
		payoff:
			"Q\\cdot\\mathbb{1}\\{\\phi(S_T-K)>0\\}\\ \\text{(cash-or-nothing)} \\qquad S_T\\cdot\\mathbb{1}\\{\\phi(S_T-K)>0\\}\\ \\text{(asset-or-nothing)}",
		assumptions: [bsDynamics, "European exercise; single observation at $T$."],
		pricingMethods: [
			{
				engine: "Analytic (only engine)",
				detail:
					"Cash-or-nothing call $=Qe^{-rT}N(d_2)$; asset-or-nothing call $=S_0e^{-qT}N(d_1)$ (and the put analogues). A vanilla call is asset-or-nothing minus $K\\times$ cash-or-nothing — the engine reuses the same $d_1,d_2$.",
			},
		],
		notes: [
			"The delta spikes near the strike as $T\\to0$ (the payoff becomes a step) — digitals are the classic example of unhedgeable pin risk.",
			"A tight call spread approximates a cash-or-nothing digital; the two should converge as the spread width → 0.",
		],
		references: [
			{
				kind: "note",
				label: "Reiner & Rubinstein (1991)",
				cite: "Unscrambling the Binary Code. Risk 4(9), 75–83.",
			},
			BLACK_SCHOLES_1973,
			{
				...HULL,
				cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson — ch. 26 (binary options).",
			},
		],

		modelDependency: {
			minimum: "Local volatility (Dupire)",
			rationale:
				"A digital is, by Breeden-Litzenberger, proportional to $-\\partial C/\\partial K$ — the *slope* of the smile at the strike, not just its level. Flat vol has zero smile slope by construction, so it misprices any digital whenever the market skew at that strike is nonzero; this is the textbook motivating example for Dupire's framework.",
		},
	},

	lookback: {
		title: "Lookback option",
		summary:
			"The strike (floating) or the settlement spot (fixed) is set to the most favourable value the underlying reached over the monitoring window. Always worth more than the corresponding vanilla — you buy hindsight.",
		payoff:
			"\\max(S_T-\\min_t S_t,\\,0)\\ \\text{(floating call)} \\qquad \\max(\\max_t S_t-K,\\,0)\\ \\text{(fixed call)}",
		assumptions: [
			bsDynamics,
			"The running extremum is tracked on the discrete simulation grid (252×T steps by default), which slightly understates the true continuous extremum.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"Steps a full path, tracks the running min/max, prices the payoff. Antithetic variates on by default. A continuity correction on the discrete extremum is not applied here — more steps reduces the bias directly.",
			},
		],
		notes: [
			"Discrete monitoring biases the price DOWN (the discrete extremum is less extreme than the continuous one) — increasing the step count should raise the price monotonically toward a limit.",
			"Floating-strike lookback delta is bounded in $[0,1]$ for a call — a useful bound to check.",
		],
		references: [
			{
				kind: "paper",
				label: "Goldman, Sosin & Gatto (1979)",
				cite: "Path Dependent Options: \u201cBuy at the Low, Sell at the High\u201d. The Journal of Finance 34(5), 1111–1127.",
			},
			{
				kind: "paper",
				label: "Conze & Viswanathan (1991)",
				cite: "Path Dependent Options: The Case of Lookback Options. The Journal of Finance 46(5), 1893–1907.",
			},
			{
				kind: "paper",
				label: "Broadie, Glasserman & Kou (1999)",
				cite: "Connecting Discrete and Continuous Path-Dependent Options. Finance and Stochastics 3(1), 55–82.",
				url: "https://doi.org/10.1007/s007800050052",
			},
		],

		modelDependency: {
			minimum: "Local volatility (Dupire)",
			rationale:
				"The running extremum means the payoff is effectively exposed to the smile across a whole range of spot levels the path may visit, not one terminal point — the same reason barrier options need it, applied to a continuum of implicit barrier levels instead of one fixed one.",
		},
	},

	basket: {
		title: "Basket option",
		summary:
			"An option on a weighted sum of several underlyings. Diversification across the basket lowers its volatility below the weighted average of the single-name vols, so a basket call is cheaper than the portfolio of single-name calls.",
		payoff: "\\max\\Big(\\textstyle\\sum_i w_i S^{(i)}_T-K,\\,0\\Big)",
		assumptions: [
			"Each underlying is GBM; the driving Brownians have a uniform pairwise correlation $\\rho$ (the full matrix is $\\rho$ off-diagonal, 1 on it).",
			"Weights and correlation are inputs; no calibration to a basket-implied surface.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"Correlated Gaussian increments are produced by a Cholesky factor of the correlation matrix; the basket value is formed at $T$ and the payoff averaged. Antithetic variates on.",
			},
		],
		notes: [
			"Basket vol $\\le\\sum_i w_i\\sigma_i$ with equality only at $\\rho=1$ — so the basket call price is monotone increasing in $\\rho$, a free check.",
			"Only delta and vega are surfaced here; per-name greeks would need one bump per underlying.",
		],
		references: [
			{
				kind: "paper",
				label: "Gentle (1993)",
				cite: "Basket Weaving. Risk 6(6), 51–52. (Geometric-average approximation for basket options.)",
			},
			{
				kind: "note",
				label: "Krekel, de Kock, Korn & Man (2004)",
				cite: "An Analysis of Pricing Methods for Basket Options. Wilmott Magazine, July 2004, 82–89.",
			},
			{
				...GLASSERMAN,
				cite: "Monte Carlo Methods in Financial Engineering, Springer — §2.3.3 (Cholesky for correlated normals).",
			},
		],

		modelDependency: {
			minimum: "Correlated Black-Scholes (a full correlation matrix)",
			rationale:
				"Even at a single terminal date, the basket's own distribution is a joint one: getting the pairwise correlation wrong is a first-order pricing error the way getting a single name's vol wrong is for a vanilla. Per-name local vol would be the next refinement if any single name's effective strike sits far from its own at-the-money.",
		},
	},

	rainbow: {
		title: "Rainbow (worst-of / best-of) option",
		summary:
			"A payoff on the worst- or best-performing underlying of a set. Worst-of options are cheap and popular in structured notes precisely because the buyer is short the correlation and short the dispersion.",
		payoff:
			"\\max\\big(\\text{perf}_{\\min/\\max}-K,\\,0\\big),\\quad \\text{perf}^{(i)}=S^{(i)}_T/S^{(i)}_0",
		assumptions: [
			"Correlated GBM underlyings, uniform pairwise $\\rho$; strike expressed as a performance level (1.0 = ATM).",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"Same correlated-path machinery as the basket; the min (or max) performance is taken across names at $T$.",
			},
		],
		notes: [
			"Worst-of price increases with $\\rho$ (names move together → the worst is less bad); best-of decreases with $\\rho$. Opposite signs — a good pair check.",
			"worst-of + best-of on two names relates to the two vanillas by a spread identity you can verify numerically.",
		],
		references: [
			{
				kind: "paper",
				label: "Stulz (1982)",
				cite: "Options on the Minimum or the Maximum of Two Risky Assets. Journal of Financial Economics 10(2), 161–185.",
				url: "https://doi.org/10.1016/0304-405X(82)90011-3",
			},
			{
				kind: "paper",
				label: "Johnson (1987)",
				cite: "Options on the Maximum or the Minimum of Several Assets. Journal of Financial and Quantitative Analysis 22(3), 277–283.",
			},
			{
				kind: "paper",
				label: "Margrabe (1978)",
				cite: "The Value of an Option to Exchange One Asset for Another. The Journal of Finance 33(1), 177–186.",
			},
		],

		modelDependency: {
			minimum: "Correlated Black-Scholes (a full correlation matrix)",
			rationale:
				"Worst-of and best-of payoffs are explicitly bets on correlation and dispersion (see the notes above) — the correlation matrix is the dominant risk factor here, ahead of any single name's smile.",
		},
	},

	future: {
		title: "Future / forward",
		summary:
			"An agreement to exchange the underlying at a fixed price $K$ at $T$. Linear in spot — no optionality, no volatility dependence.",
		payoff: "N\\cdot(S_T-K)",
		assumptions: [
			"Cost-of-carry: the fair forward is $F=S_0e^{(r-q)T}$.",
			"Deterministic rates and dividend yield.",
		],
		pricingMethods: [
			{
				engine: "Analytic (only engine)",
				detail:
					"Present value $=N(S_0e^{-qT}-Ke^{-rT})$. Delta is $Ne^{-qT}$, rho is linear in $T$; gamma, vega, theta (ex-carry) are zero.",
			},
		],
		notes: [
			"At $K=F$ the contract is worth zero at inception — the standard forward convention.",
			"This is the linear leg used inside collars and covered calls in the strategy visualiser.",
		],
		references: [
			{
				...HULL,
				cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson — ch. 5 (forwards & futures pricing).",
			},
			{
				kind: "paper",
				label: "Cox, Ingersoll & Ross (1981)",
				cite: "The Relation between Forward Prices and Futures Prices. Journal of Financial Economics 9(4), 321–346.",
			},
		],

		modelDependency: {
			minimum: "None beyond the discount/carry curve",
			rationale:
				'The payoff is linear in $S_T$, so no distributional assumption — vol, smile, jumps — enters the price at all. This is the one entry in the catalog where "minimum model" is genuinely "no stochastic model."',
		},
	},

	bond: {
		title: "Fixed-rate / zero-coupon bond",
		summary:
			"A stream of fixed coupons plus principal at maturity (fixed-rate), or a single discounted principal (zero-coupon). Priced by discounting cash flows on a flat or supplied curve.",
		payoff:
			"P=\\sum_i c\\,N\\,e^{-r t_i}+N e^{-r T}\\quad(\\text{zero-coupon: }c=0)",
		assumptions: [
			"Flat continuously-compounded rate $r$ unless explicit discount times/factors are supplied.",
			"No credit spread, no optionality (not callable/putable).",
		],
		pricingMethods: [
			{
				engine: "Analytic (only engine)",
				detail:
					"Discounts each coupon date and the redemption. The bond-analytics block returns Macaulay & modified duration, convexity and DV01 from the same cash-flow schedule.",
			},
		],
		notes: [
			"At a coupon rate equal to the discount rate a fixed-rate bond prices at par ($P=N$) — a free check.",
			"DV01 ≈ modified duration × price × 0.0001 — the two analytics fields should be mutually consistent.",
		],
		references: [
			{
				kind: "book",
				label: "Fabozzi (2021)",
				cite: "Bond Markets, Analysis, and Strategies, 10th ed., MIT Press — ch. 2–4 (pricing, yield, duration & convexity).",
			},
			{
				kind: "book",
				label: "Tuckman & Serrat (2011)",
				cite: "Fixed Income Securities: Tools for Today's Markets, 3rd ed., Wiley.",
			},
			{
				...HULL,
				cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson — ch. 4 (interest rates, duration, convexity).",
			},
		],

		modelDependency: {
			minimum: "A deterministic discount curve",
			rationale:
				"Cash flows are fixed and known; the only market input is how they are discounted. A stochastic short-rate model (Vasicek/Hull-White) only becomes necessary for embedded optionality — a call/put feature this catalog entry does not price.",
		},
	},

	autocall: {
		title: "Autocallable note",
		summary:
			"A structured note on one underlying that redeems early with a coupon if the spot is at or above a barrier on any observation date, and otherwise exposes the investor to downside below a knock-in barrier if it survives to maturity.",
		payoff: [
			"\\text{At } T_i:\\quad S(T_i)\\ge B_{ac}S_0 \\;\\Longrightarrow\\; N(1+c_i)",
			"\\text{If alive at } T_n:\\ \\begin{cases} N(1+c_n) & S(T_n)\\ge B_{cpn}S_0 \\\\ N & B_{ki}S_0\\le S(T_n)<B_{cpn}S_0 \\\\ N\\,S(T_n)/S_0 & S(T_n)<B_{ki}S_0 \\end{cases}",
		],
		assumptions: [
			"Single underlying, Black-Scholes GBM. Barriers are fractions of $S_0$.",
			"Memory coupons (default on): a coupon missed earlier is paid later if a subsequent date clears the coupon barrier.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"The spot is simulated exactly at each observation date via the closed-form GBM transition — no daily path needed. Autocall, coupon and knock-in checks all read the same simulated path.",
			},
		],
		notes: [
			'The "continuous knock-in" flag does NOT apply a Brownian-bridge correction — it only checks the knock-in at every observation date instead of only the last. Read "continuous" as "checked more often".',
			"Economically the investor is short a knock-in put struck at $B_{ki}S_0$ dressed with a coupon — the coupon can make the product look lower-risk than the downside it carries.",
		],
		references: [
			{
				kind: "book",
				label: "Bouzoubaa & Osseiran (2010)",
				cite: "Exotic Options and Hybrids: A Guide to Structuring, Pricing and Trading, Wiley — ch. 16 (autocallables).",
			},
			{
				kind: "paper",
				label: "Deng, Mallett & McCann (2011)",
				cite: "Modeling Autocallable Structured Products. Journal of Derivatives & Hedge Funds 17(4), 326–340.",
			},
			{
				kind: "book",
				label: "Overhaus et al. (2007)",
				cite: "Equity Hybrid Derivatives, Wiley.",
			},
		],

		modelDependency: {
			minimum: "Local volatility (Dupire) at minimum; SLV is the desk standard",
			rationale:
				"An autocall is a strip of barrier-like observations with coupon memory, so it inherits barrier risk's smile-slope sensitivity at every observation date. In practice this is one of the textbook motivating cases for stochastic-local vol specifically: an autocall book's vega and gamma are highly sensitive to *how* the smile is assumed to move forward (sticky-strike vs. sticky-delta), which local vol alone answers in a fixed, model-specific — not necessarily realistic — way.",
		},
	},

	mountain: {
		title: "Mountain range (Himalaya) option",
		summary:
			"A multi-asset, multi-period structured option. On each observation date the best-performing remaining underlying is locked in and removed from the basket; the payoff is built from the recorded performances.",
		payoff:
			"\\max\\Big(\\tfrac1m\\sum_{k=1}^{m}\\text{perf}^{(\\text{best at }T_k)}-K,\\,0\\Big)",
		assumptions: [
			"Correlated GBM underlyings with a full supplied correlation matrix.",
			"One asset is removed per observation date (Himalaya rule).",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"Correlated paths (Cholesky), then the removal rule is applied per path and the locked performances averaged.",
			},
		],
		notes: [
			"Currently disabled in the catalog — the engine is not yet vetted.",
			"Sensitivity to the correlation matrix is large and non-monotone; this product is a stress test of the correlated-path code.",
		],
		references: [
			{
				kind: "book",
				label: "Overhaus et al. (2007)",
				cite: "Equity Hybrid Derivatives, Wiley — multi-asset structured payoffs including mountain ranges (Himalaya, Everest, Altiplano).",
			},
			{
				kind: "note",
				label: "Quessette (2002)",
				cite: "New Products, New Risks. Risk 15(3), 97–100. (Pricing and hedging mountain-range options.)",
			},
			{
				kind: "book",
				label: "Bouzoubaa & Osseiran (2010)",
				cite: "Exotic Options and Hybrids, Wiley — ch. 14–15 (worst-of and mountain-range structures).",
			},
		],

		modelDependency: {
			minimum: "Correlated local volatility",
			rationale:
				"Combines mountain range's own two demands at once: a full correlation matrix (as for basket/rainbow, since the removal rule depends on relative performance across names) and per-date smile sensitivity (as for barrier/autocall, since each observation locks in a level-dependent performance). The richest minimum requirement in this catalog.",
		},
	},

	"variance-swap": {
		title: "Variance swap",
		summary:
			"A forward on realised variance: pays the difference between realised annualised variance over $[0,T]$ and a fixed strike, times a variance notional.",
		payoff:
			"N_{\\text{var}}\\big(\\sigma_R^2-K_{\\text{var}}\\big),\\quad \\sigma_R^2=\\tfrac{A}{n}\\sum_i\\ln^2\\!\\frac{S_{t_i}}{S_{t_{i-1}}}",
		assumptions: [
			"Log-return variance estimator, annualisation factor $A$; discrete sampling.",
			"The analytic engine uses the replication result: variance swap fair strike = a strip of OTM options weighted $1/K^2$.",
		],
		pricingMethods: [
			{
				engine: "Analytic",
				detail:
					"Static replication of the log contract by a continuum of vanillas; under flat Black-Scholes this returns $\\sigma^2$ exactly.",
			},
			{
				engine: "Monte-Carlo",
				detail:
					"Simulates the path, forms the realised-variance estimator, discounts the swap payoff.",
			},
		],
		notes: [
			"Under flat Black-Scholes the fair strike equals $\\sigma^2$ regardless of sampling frequency in the continuous limit — the MC and analytic engines should agree within noise.",
			"Currently disabled — not yet vetted.",
		],
		references: [
			{
				kind: "note",
				label: "Demeterfi, Derman, Kamal & Zou (1999)",
				cite: "More Than You Ever Wanted to Know About Volatility Swaps. Goldman Sachs Quantitative Strategies Research Notes. (The replication argument implemented in the analytic engine.)",
			},
			{
				kind: "paper",
				label: "Carr & Madan (1998)",
				cite: "Towards a Theory of Volatility Trading. In: Volatility: New Estimation Techniques for Pricing Derivatives (R. Jarrow, ed.), Risk Books, 417–427.",
			},
			{
				kind: "book",
				label: "Gatheral (2006)",
				cite: "The Volatility Surface: A Practitioner's Guide, Wiley — ch. 11 (variance & volatility swaps).",
			},
		],

		modelDependency: {
			minimum: "The full market smile (a strip of vanillas), not a single vol",
			rationale:
				"The replication argument this engine implements *is* the point: a variance swap's fair strike is a weighted integral of OTM option prices across every strike, so it is directly exposed to the whole smile's shape, not one point on it. Flat vol only happens to equal $\\sigma^2$ because there is no smile left to integrate over.",
		},
	},

	"volatility-swap": {
		title: "Volatility swap",
		summary:
			"A forward on realised volatility (not variance). Pays $N_{\\text{vol}}(\\sigma_R-K_{\\text{vol}})$. The concavity of $\\sqrt{\\cdot}$ makes it worth strictly less than the square root of the variance-swap strike (the convexity adjustment).",
		payoff: "N_{\\text{vol}}\\big(\\sigma_R-K_{\\text{vol}}\\big)",
		assumptions: [
			"Same realised estimator as the variance swap; no closed form because of the square root.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"Simulates paths, forms $\\sqrt{\\text{realised variance}}$ per path, averages.",
			},
		],
		notes: [
			"$K_{\\text{vol}}<\\sqrt{K_{\\text{var}}}$ always (Jensen) — the gap is the convexity adjustment, a free ordering check against the variance swap.",
			"Currently disabled — not yet vetted.",
		],
		references: [
			{
				kind: "note",
				label: "Demeterfi, Derman, Kamal & Zou (1999)",
				cite: "More Than You Ever Wanted to Know About Volatility Swaps. Goldman Sachs Quantitative Strategies Research Notes.",
			},
			{
				kind: "paper",
				label: "Broadie & Jain (2008)",
				cite: "The Effect of Jumps and Discrete Sampling on Volatility and Variance Swaps. International Journal of Theoretical and Applied Finance 11(8), 761–797.",
			},
			{
				kind: "paper",
				label: "Carr & Lee (2009)",
				cite: "Robust Replication of Volatility Derivatives. PRMIA Award paper / Columbia & Chicago working paper.",
			},
		],

		modelDependency: {
			minimum: "A dynamic volatility model (Heston or SLV)",
			rationale:
				"Unlike the variance swap, there is no model-free static replication for the square root of realised variance — the convexity adjustment between $\\sqrt{\\mathbb{E}[\\sigma_R^2]}$ and $\\mathbb{E}[\\sigma_R]$ genuinely depends on the *dynamics* of volatility, not just today's smile, which is exactly what a static (local-vol) model cannot supply.",
		},
	},

	"dispersion-swap": {
		title: "Dispersion swap",
		summary:
			"Trades the spread between index variance and the weighted sum of single-name variances — a direct bet on correlation. Long dispersion = short correlation.",
		payoff:
			"N\\Big(\\sum_i w_i\\sigma_{R,i}^2-\\sigma_{R,\\text{index}}^2-K_{\\text{spread}}\\Big)",
		assumptions: [
			"Correlated GBM underlyings, uniform pairwise $\\rho$; the index is the weighted sum.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (only engine)",
				detail:
					"Correlated paths; realised variance of each name and of the index are formed on the same paths and the spread payoff averaged.",
			},
		],
		notes: [
			"The spread is a monotone decreasing function of $\\rho$ — at $\\rho=1$ index variance equals the weighted single-name variance and the spread collapses.",
			"Currently disabled — not yet vetted.",
		],
		references: [
			{
				kind: "note",
				label: "Bossu (2007)",
				cite: "A New Approach for Modelling and Pricing Correlation Swaps. Dresdner Kleinwort Equity Derivatives working paper.",
			},
			{
				kind: "book",
				label: "Bossu (2014)",
				cite: "Advanced Equity Derivatives: Volatility and Correlation, Wiley.",
			},
			{
				kind: "paper",
				label: "Driessen, Maenhout & Vilkov (2009)",
				cite: "The Price of Correlation Risk: Evidence from Equity Options. The Journal of Finance 64(3), 1377–1406.",
			},
		],

		modelDependency: {
			minimum:
				"Correlated variance model (per-name variance dynamics + a full correlation matrix)",
			rationale:
				"A dispersion swap is explicitly a correlation trade on realised variance, not price — it needs both a variance-swap-consistent model per name (see that entry) and the joint correlation structure linking them, the variance analogue of what a basket needs for price.",
		},
	},

	"fx-forward": {
		title: "FX forward",
		summary:
			"An agreement to exchange one currency for another at a fixed rate at $T$. Priced by covered interest parity using the two currencies' rates.",
		payoff: "N\\,(S_T-K)\\ \\text{(domestic per foreign)}",
		assumptions: [
			"Garman-Kohlhagen: the foreign rate plays the role of a dividend yield. Fair forward $=S_0e^{(r_d-r_f)T}$.",
		],
		pricingMethods: [
			{
				engine: "Analytic (only engine)",
				detail: "Present value $=N(S_0e^{-r_fT}-Ke^{-r_dT})$.",
			},
		],
		notes: [
			"The forward points $S_0(e^{(r_d-r_f)T}-1)$ are positive when the domestic rate exceeds the foreign one.",
			"Currently disabled — not yet vetted.",
		],
		references: [
			{
				...HULL,
				cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson — ch. 5 (covered interest parity).",
			},
			{
				kind: "book",
				label: "Clark (2011)",
				cite: "Foreign Exchange Option Pricing: A Practitioner's Guide, Wiley — ch. 2–3.",
			},
		],

		modelDependency: {
			minimum: "None beyond the two rate curves",
			rationale:
				"Linear in the spot exchange rate, exactly like an equity forward — covered interest parity fixes the fair rate from the two currencies' discount curves alone, no volatility model needed.",
		},
	},

	"fx-option": {
		title: "FX option",
		summary:
			"A European call or put on an exchange rate. Black-Scholes with the foreign rate as the dividend yield (Garman-Kohlhagen).",
		payoff: "N\\max(\\phi(S_T-K),0),\\quad \\phi=\\pm1",
		assumptions: [
			"Garman-Kohlhagen dynamics; constant $r_d$, $r_f$, $\\sigma$.",
		],
		pricingMethods: [
			{
				engine: "Analytic (only engine)",
				detail:
					"Black-Scholes with $q\\to r_f$. A call to buy foreign currency is a put on the inverse quote — the same formula, reciprocated.",
			},
		],
		notes: [
			"Put-call parity holds with $S_0e^{-r_fT}-Ke^{-r_dT}$.",
			"Currently disabled — not yet vetted.",
		],
		references: [
			{
				kind: "paper",
				label: "Garman & Kohlhagen (1983)",
				cite: "Foreign Currency Option Values. Journal of International Money and Finance 2(3), 231–237.",
			},
			{
				kind: "book",
				label: "Clark (2011)",
				cite: "Foreign Exchange Option Pricing: A Practitioner's Guide, Wiley.",
			},
		],

		modelDependency: {
			minimum:
				"Flat Garman-Kohlhagen for one quoted vol; SABR for a consistent FX book",
			rationale:
				"Formally identical to an equity vanilla (flat vol matches one quoted strike/maturity exactly), but FX smiles are typically far steeper and more symmetric-skewed (risk reversals, butterflies) than equity ones — SABR, already in this repo, is the standard minimum for pricing an FX book consistently across the whole smile rather than one strike at a time.",
		},
	},

	"commodity-forward": {
		title: "Commodity forward",
		summary:
			"A forward on a commodity, where the carry includes storage cost and a convenience yield (the benefit of holding the physical good).",
		payoff: "N\\,(S_T-K)",
		assumptions: [
			"Fair forward $=S_0e^{(r+u-y)T}$ with storage cost $u$ and convenience yield $y$.",
		],
		pricingMethods: [
			{
				engine: "Analytic (only engine)",
				detail:
					"Present value $=N(S_0e^{(u-y)T}e^{-rT}\\cdot e^{rT}-Ke^{-rT})$ — i.e. discount the carry-adjusted forward.",
			},
		],
		notes: [
			"A high convenience yield produces backwardation (forward below spot); high storage cost produces contango.",
			"Currently disabled — not yet vetted.",
		],
		references: [
			{
				kind: "paper",
				label: "Schwartz (1997)",
				cite: "The Stochastic Behavior of Commodity Prices: Implications for Valuation and Hedging. The Journal of Finance 52(3), 923–973.",
			},
			{
				kind: "book",
				label: "Geman (2005)",
				cite: "Commodities and Commodity Derivatives: Modeling and Pricing for Agriculturals, Metals and Energy, Wiley.",
			},
		],

		modelDependency: {
			minimum: "None beyond the carry curve (storage cost, convenience yield)",
			rationale:
				"Linear in $S_T$ — the only model content is the cost-of-carry curve; no distributional assumption is needed, same as any other forward.",
		},
	},

	"commodity-option": {
		title: "Commodity option",
		summary:
			"A European option on a commodity forward/spot, Black-Scholes with a carry term combining storage cost and convenience yield.",
		payoff: "N\\max(\\phi(S_T-K),0)",
		assumptions: ["Constant $\\sigma$; net carry $b=r+u-y$ enters the drift."],
		pricingMethods: [
			{
				engine: "Analytic (only engine)",
				detail:
					"Black-76 style: price the option on the carry-adjusted forward, discount at $r$.",
			},
		],
		notes: [
			"Reduces to the equity option when $u=y=0$.",
			"Currently disabled — not yet vetted.",
		],
		references: [
			BLACK_1976,
			{
				kind: "paper",
				label: "Schwartz (1997)",
				cite: "The Stochastic Behavior of Commodity Prices: Implications for Valuation and Hedging. The Journal of Finance 52(3), 923–973.",
			},
			{
				kind: "book",
				label: "Geman (2005)",
				cite: "Commodities and Commodity Derivatives, Wiley.",
			},
		],

		modelDependency: {
			minimum:
				"Flat Black-76 for one quoted vol; a mean-reverting spot model (Schwartz-Smith) for a consistent book",
			rationale:
				"A single quoted strike is matched exactly by a flat vol on the carry-adjusted forward, same as any vanilla. But commodity spot dynamics are famously mean-reverting and seasonal (see the Schwartz-Smith entry in this catalog's timeline/AAD models) — pricing several maturities consistently needs that mean reversion, not a flat-vol GBM extrapolation of a single observed vol.",
		},
	},

	"forward-start": {
		title: "Forward-start option",
		summary:
			"A vanilla option whose strike is only fixed at a future date $T_0$, as a fraction $\\alpha$ of the spot prevailing then, with exercise at a later maturity $T>T_0$. The building block cliquets and napoleons are built from, one reset at a time.",
		payoff: "\\max(\\phi(S_T-\\alpha S_{T_0}),0),\\quad \\phi=\\pm1",
		assumptions: [
			bsDynamics,
			"Strike-setting date $T_0$ and moneyness ratio $\\alpha$ (1.0 = at-the-money-forward) fixed at inception.",
		],
		pricingMethods: [
			{
				engine: "Closed form under flat Black-Scholes",
				detail:
					"Under GBM, $S_T/S_{T_0}$ is independent of $S_{T_0}$, so the forward-start price is exactly $S_0e^{-qT_0}$ times a vanilla priced on the residual maturity $T-T_0$ at moneyness $\\alpha$ — no simulation needed under flat vol.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"The scaling trick above is exact only under flat vol; it is also the reason forward-start risk is a clean probe of forward-skew assumptions (see modelDependency).",
		],
		references: [
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids: A Guide to Structuring, Pricing and Trading, Wiley — ch. 6 (forward-start options).",
			},
			{
				kind: "paper",
				label: "Rubinstein (1991)",
				cite: "Pay Now, Choose Later. Risk 4(2), 44–47. (Forward-start and related deferred-strike structures.)",
			},
		],
		modelDependency: {
			minimum:
				"Stochastic volatility (Heston or SLV) — local vol is a known trap here",
			rationale:
				"The flat-vol closed form is exact only because $S_T/S_{T_0}$ decouples from $S_{T_0}$ — a property that survives in local vol only if the smile does not change shape as spot moves, which Dupire local vol famously violates (it flattens the forward skew as time passes). Since the whole point of a forward-start is exposure to the smile *at a future date*, a model that gets the forward smile wrong misprices it by construction; stochastic vol is the standard fix.",
		},
	},

	compound: {
		title: "Compound option",
		summary:
			"An option on an option: the right to buy (or sell) a vanilla option at a fixed price on an intermediate date $T_1$, with the underlying vanilla itself expiring at $T_2>T_1$. Four flavours: call-on-call, call-on-put, put-on-call, put-on-put.",
		payoff:
			"\\max\\big(\\phi_1(V_{\\text{BS}}(S_{T_1},K_2,T_2-T_1)-K_1),0\\big),\\quad \\phi_1=\\pm1",
		assumptions: [
			bsDynamics,
			"Both strikes ($K_1$ on the compound, $K_2$ on the underlying vanilla) and both maturities fixed at inception.",
		],
		pricingMethods: [
			{
				engine: "Closed form (Geske)",
				detail:
					"A bivariate-normal generalisation of Black-Scholes: the compound's exercise boundary at $T_1$ is the spot level where the underlying vanilla's Black-Scholes value equals $K_1$, found by a 1-D root search, then folded into a bivariate normal CDF.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"Compound options are the standard building block for pricing installment options and some convertible-bond optionality.",
		],
		references: [
			{
				kind: "paper",
				label: "Geske (1979)",
				cite: "The Valuation of Compound Options. Journal of Financial Economics 7(1), 63–81.",
				url: "https://doi.org/10.1016/0304-405X(79)90022-9",
			},
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids, Wiley — ch. 7 (compound options).",
			},
		],
		modelDependency: {
			minimum: "Local volatility (Dupire)",
			rationale:
				"The exercise decision at $T_1$ depends on the underlying vanilla's value at whatever spot level prevails then — i.e. on the smile at $T_2-T_1$ residual maturity as seen FROM $T_1$, across a range of possible $S_{T_1}$ levels around the exercise boundary. A flat vol gets the level right only at the strike it was calibrated to; local vol reproduces today's smile at each relevant strike/maturity pair.",
		},
	},

	chooser: {
		title: "Chooser option",
		summary:
			"The holder pays for the right to decide, on a choice date $T_1$, whether the option is a call or a put with common strike $K$ and common final maturity $T_2$. A 'simple' chooser has both legs at the same strike; a 'complex' chooser lets the call/put strikes and maturities differ.",
		payoff:
			"\\max\\big(V_{\\text{call}}(S_{T_1}),\\,V_{\\text{put}}(S_{T_1})\\big)\\ \\text{evaluated via put-call parity at } T_1",
		assumptions: [
			bsDynamics,
			"Simple chooser: common strike and maturity for both legs, which reduces the payoff to a call plus a scaled put via put-call parity.",
		],
		pricingMethods: [
			{
				engine: "Closed form (simple chooser)",
				detail:
					"Put-call parity turns the max at $T_1$ into a call struck at $K$ maturing $T_2$, plus $e^{-q(T_2-T_1)}$ puts struck at $Ke^{-(r-q)(T_2-T_1)}$ maturing $T_1$ — two ordinary Black-Scholes legs, no bivariate normal needed.",
			},
			{
				engine: "Closed form (complex chooser, bivariate normal)",
				detail:
					"When the call/put strikes or maturities differ, the choice boundary at $T_1$ requires the same bivariate-normal machinery as a compound option.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"A simple chooser is always worth more than a straddle with the same strike/maturity struck at $T_1$, since the holder chooses AFTER $T_1$ information but before either leg's own maturity.",
		],
		references: [
			{
				kind: "paper",
				label: "Rubinstein (1991)",
				cite: "Options for the Undecided. Risk 4(4), 70–73. (Simple and complex chooser options.)",
			},
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids, Wiley — ch. 7 (chooser options).",
			},
		],
		modelDependency: {
			minimum:
				"Flat Black-Scholes (simple chooser); stochastic volatility for the complex chooser",
			rationale:
				"The simple chooser reduces to two vanilla legs at the SAME strike via put-call parity, so it inherits the vanilla's one-point-on-the-smile sufficiency. The complex chooser does not reduce this way — the choice is between genuinely different strikes/maturities, which is a bet on how the smile will have moved by $T_1$, the same forward-smile dependency that pushes compound options and cliquets toward stochastic vol.",
		},
	},

	cliquet: {
		title: "Cliquet (ratchet) option",
		summary:
			"A strip of forward-start options with successive reset dates $T_0=0<T_1<\\dots<T_n$: at each reset the strike for the NEXT period is set at (or near) the then-prevailing spot, and the period returns are summed (globally floored/capped) into a single final payoff. Monetises realised volatility period by period rather than terminal spot.",
		payoff:
			"N\\max\\Big(F_{\\text{floor}},\\,\\min\\Big(F_{\\text{cap}},\\,\\sum_{i=1}^{n}\\text{clip}\\big(R_i,\\,\\ell,\\,u\\big)\\Big)\\Big),\\quad R_i=\\frac{S_{T_i}}{S_{T_{i-1}}}-1",
		assumptions: [
			bsDynamics,
			"Local caps/floors $[\\ell,u]$ clip each period's return before summation; global floor/cap $[F_{\\text{floor}},F_{\\text{cap}}]$ bounds the total.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (standard approach)",
				detail:
					"Simulate the path through every reset date, form each period's clipped return, sum, apply the global floor/cap, discount. No closed form once local caps/floors are active.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"Built entirely from forward-start legs (one per period) — see that entry for the single-period building block and its forward-skew sensitivity.",
		],
		references: [
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids, Wiley — ch. 15 (cliquets and ratchet options).",
			},
			{
				kind: "note",
				label: "Wilmott (2002)",
				cite: "Cliquet Options and Volatility Models. Wilmott Magazine, December 2002.",
			},
		],
		modelDependency: {
			minimum: "Stochastic volatility (Heston or SLV)",
			rationale:
				"Each period is a forward-start option, and the sum over many periods compounds that dependence on the FORWARD smile rather than today's — precisely where local vol is known to be structurally wrong (it flattens forward skew as time advances) while flat vol has no skew at all. This is the standard textbook example motivating stochastic vol over local vol for exotic desks.",
		},
	},

	napoleon: {
		title: "Napoleon option",
		summary:
			"A cliquet variant where each period's coupon is a FIXED rate minus the worst (most negative) monthly return observed within that period, floored at zero, summed across periods. Effectively a short-volatility cliquet: the investor is paid a fixed coupon reduced by the worst realised drawdown.",
		payoff:
			"N\\sum_{i=1}^{n}\\max\\Big(c-\\big(-\\min_{j\\in\\text{period }i}R_j\\big),\\,0\\Big)",
		assumptions: [
			bsDynamics,
			"Sub-period (e.g. monthly) returns $R_j$ observed within each annual period; only the worst (most negative) one matters per period.",
		],
		pricingMethods: [
			{
				engine: "Monte-Carlo (standard approach)",
				detail:
					"Simulate through every sub-period observation date, track the running minimum return within each period, form the coupon, sum and discount.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"The worst-of-period-returns structure makes this short volatility and short skew at once — a stress test even sharper than the plain cliquet for forward-smile assumptions.",
		],
		references: [
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids, Wiley — ch. 15 (Napoleon and other cliquet-family structures).",
			},
		],
		modelDependency: {
			minimum: "Stochastic volatility (Heston or SLV)",
			rationale:
				"Same forward-smile dependence as the cliquet, sharpened: the payoff singles out the single WORST sub-period return per period, an extreme-value functional of the path that is far more sensitive to the tails of the forward return distribution — and hence to how vol-of-vol and skew are assumed to evolve — than a simple sum of clipped returns.",
		},
	},

	quanto: {
		title: "Quanto option",
		summary:
			"An option on a foreign-currency underlying, settled in domestic currency at a FIXED exchange rate agreed at inception — the currency conversion is pre-agreed, so there is no FX risk on the payoff itself, only a drift adjustment from the correlation between the underlying and the exchange rate.",
		payoff:
			"N_{\\text{fx}}\\max(\\phi(S_T^{\\text{foreign}}-K),0),\\quad \\phi=\\pm1\\ \\text{(struck and settled in domestic currency)}",
		assumptions: [
			"Underlying follows GBM in its own (foreign) currency; the quanto drift adjustment shifts that drift by $-\\rho\\sigma_S\\sigma_{\\text{fx}}$, where $\\rho$ is the correlation between the underlying's and the exchange rate's returns.",
			"Constant correlation, constant volatilities for both legs.",
		],
		pricingMethods: [
			{
				engine: "Closed form (quanto-adjusted Black-Scholes)",
				detail:
					"Ordinary Black-Scholes on the foreign underlying with its drift replaced by $r_d-q-\\rho\\sigma_S\\sigma_{\\text{fx}}$ (domestic rate, dividend yield, and the quanto correlation adjustment), discounted at the domestic rate.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"A genuinely closed-form product: the correlation adjustment is a single extra drift term, no numerical method beyond ordinary Black-Scholes required.",
		],
		references: [
			{
				kind: "paper",
				label: "Reiner (1992)",
				cite: "Quanto Mechanics. Risk 5(3), 59–63.",
			},
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids, Wiley — ch. 11 (quanto options).",
			},
			{
				...HULL,
				cite: "Options, Futures, and Other Derivatives, 11th ed., Pearson — ch. 32 (quantos).",
			},
		],
		modelDependency: {
			minimum: "Flat Black-Scholes plus one correlation input",
			rationale:
				"The quanto adjustment is a closed-form drift shift, not a distributional change — the underlying is still lognormal under the quanto-adjusted measure. A single quoted vol on the underlying and a single quoted correlation to the FX rate are genuinely sufficient; there is no smile-shape dependence beyond what a plain vanilla already has.",
		},
	},

	"double-barrier": {
		title: "Double barrier option",
		summary:
			"A vanilla option that knocks in or out if the spot touches EITHER of two barriers, one above and one below the current spot, before maturity. Cheaper than a single-barrier option of the same type since there are two ways to trigger.",
		payoff:
			"\\max(\\phi(S_T-K),0)\\cdot\\mathbb{1}\\{\\text{neither barrier touched}\\}\\ \\ \\text{(double knock-out)}",
		assumptions: [
			"Same Black-Scholes GBM dynamics as the single-barrier option.",
			"Monitoring frequency (discrete vs. continuous) materially affects the corridor's effective width, more so than for a single barrier.",
		],
		pricingMethods: [
			{
				engine: "Closed form (Kunitomo-Ikeda series)",
				detail:
					"An infinite series of image-charge reflections between the two barriers (the method-of-images generalisation of the single-barrier closed form), truncated once terms are negligible.",
			},
			{
				engine: "Monte-Carlo, with Brownian-bridge correction",
				detail:
					"Simulate at each monitoring date; a Brownian-bridge check against BOTH barriers on every interval corrects the discrete-monitoring bias, the two-barrier analogue of the single-barrier engine already in this repo.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"Reduces to the existing single-barrier entry as either barrier is pushed to $\\pm\\infty$ — a useful consistency check once implemented.",
		],
		references: [
			{
				kind: "paper",
				label: "Kunitomo & Ikeda (1992)",
				cite: "Pricing Options with Curved Boundaries. Mathematical Finance 2(4), 275–298.",
				url: "https://doi.org/10.1111/j.1467-9965.1992.tb00033.x",
			},
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids, Wiley — ch. 5 (double barrier options).",
			},
		],
		modelDependency: {
			minimum:
				"Local volatility (Dupire); stochastic-local vol for hedging risk",
			rationale:
				"Two barrier levels means two OTM smile points now drive the price instead of one — the same slope sensitivity that pushes a single barrier past flat vol, doubled. SLV remains the desk standard for the same forward-skew reason as the single-barrier entry, since spot can approach either barrier as the smile itself evolves.",
		},
	},

	corridor: {
		title: "Corridor / range accrual option",
		summary:
			"Pays a fixed daily (or periodic) amount for each observation date the underlying spends INSIDE a specified range $[L,U]$, accrued and paid at maturity — a bet on realised range-bound behaviour rather than direction.",
		payoff:
			"N\\cdot\\frac{1}{n}\\sum_{i=1}^{n}\\mathbb{1}\\{L\\le S_{t_i}\\le U\\}",
		assumptions: [
			bsDynamics,
			"Discrete daily (or periodic) observation of whether spot is inside the corridor; no path dependency beyond the indicator count.",
		],
		pricingMethods: [
			{
				engine: "Closed form (sum of digitals)",
				detail:
					"Under flat vol and INDEPENDENT observation dates (no path dependency in the indicator itself, only in each date's own marginal), the accrual is exactly the average of $n$ two-touch digital probabilities, each priced from the terminal-date marginal via $N(d_2^U)-N(d_2^L)$.",
			},
			{
				engine: "Monte-Carlo",
				detail:
					"Simulate the path through every observation date, average the corridor indicator, discount — needed once observation dates are correlated through a genuinely path-dependent feature (e.g. a running range) rather than independent marginals.",
			},
		],
		notes: [
			"No pricing engine in this repo yet — documentation only.",
			"The closed-form sum-of-digitals price is exact per-date but ignores the joint (path) distribution across dates — fine for the expectation since the indicator is linear in each date's own marginal, but NOT sufficient for path-dependent Greeks like day-to-day range risk.",
		],
		references: [
			{
				...BOUZOUBAA,
				cite: "Exotic Options and Hybrids, Wiley — ch. 9 (range accrual / corridor options).",
			},
			{
				kind: "note",
				label: "Reiner & Rubinstein (1991)",
				cite: "Unscrambling the Binary Code. Risk 4(9), 75–83. (The two-touch digital building block.)",
			},
		],
		modelDependency: {
			minimum: "Local volatility (Dupire)",
			rationale:
				"Each observation date's contribution is a two-touch digital, which — exactly like the single-strike digital already in this catalog — is a bet on the smile's SLOPE at both range edges, not its level. A flat vol gets the width of the corridor systematically wrong whenever the market skew is nonzero at either edge; local vol matches the smile at each relevant date and strike.",
		},
	},
};
