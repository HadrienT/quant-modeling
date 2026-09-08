/**
 * Product doc cards — blueprint WP 99. Content describes THIS repo's engines
 * (src/engines/, src/instruments/), not pricing theory in the abstract. It
 * ages like a docstring: re-check each "How it's priced here" bullet against the
 * engine source whenever that engine changes.
 *
 * The `option`, `barrier` and `autocall` entries are the recovered work
 * (blueprint WP 99); the rest are written from the request schemas and engine
 * layout and are worth a maintainer pass against src/ before being treated as
 * authoritative.
 */

export type PricingMethodEntry = { engine: string; detail: string };

export type ProductDoc = {
	title: string;
	summary: string;
	/** one or more KaTeX block formulas */
	payoff: string | string[];
	assumptions: string[];
	pricingMethods: PricingMethodEntry[];
	notes: string[];
};

const bsDynamics =
	"The underlying follows geometric Brownian motion under the risk-neutral measure, $dS_t=(r-q)S_t\\,dt+\\sigma S_t\\,dW_t$, with constant $r$, $q$ and $\\sigma$ (flat vol — no smile, no term structure).";

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
	},
};
