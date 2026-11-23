import type { Reference } from "./docs";

/**
 * The bibliography of the numerical methods behind a product's pricing (the
 * Products page's "How it is implemented" panel): the paper or chapter each
 * method of the engine comes from. Same rules as docs.ts: a `url` only where
 * the DOI was checked, a citation complete enough to find the work without
 * one. A key is cited in the panel's prose as `[@key]`.
 */
export const METHOD_REFS = {
	sobol: {
		kind: "paper",
		label: "Sobol' (1967)",
		cite: "On the distribution of points in a cube and the approximate evaluation of integrals. USSR Computational Mathematics and Mathematical Physics 7(4), 86–112.",
		url: "https://doi.org/10.1016/0041-5553(67)90144-9",
	},
	joeKuo: {
		kind: "paper",
		label: "Joe & Kuo (2008)",
		cite: "Constructing Sobol sequences with better two-dimensional projections. SIAM Journal on Scientific Computing 30(5), 2635–2654.",
		url: "https://doi.org/10.1137/070709359",
	},
	lecuyerLemieux: {
		kind: "paper",
		label: "L'Ecuyer & Lemieux (2002)",
		cite: "Recent advances in randomized quasi-Monte Carlo methods. In Dror, L'Ecuyer & Szidarovszky (eds.), Modeling Uncertainty, Springer, 419–474.",
		url: "https://doi.org/10.1007/0-306-48102-2_20",
	},
	moskowitzCaflisch: {
		kind: "paper",
		label: "Moskowitz & Caflisch (1996)",
		cite: "Smoothness and dimension reduction in quasi-Monte Carlo methods. Mathematical and Computer Modelling 23(8–9), 37–54.",
		url: "https://doi.org/10.1016/0895-7177(96)00038-6",
	},
	wichura: {
		kind: "paper",
		label: "Wichura (1988)",
		cite: "Algorithm AS 241: The percentage points of the normal distribution. Journal of the Royal Statistical Society, Series C (Applied Statistics) 37(3), 477–484.",
		url: "https://doi.org/10.2307/2347330",
	},
	salmon: {
		kind: "paper",
		label: "Salmon, Moraes, Dror & Shaw (2011)",
		cite: "Parallel random numbers: as easy as 1, 2, 3. Proceedings of SC11, the International Conference for High Performance Computing, Networking, Storage and Analysis, 1–12.",
		url: "https://doi.org/10.1145/2063384.2063405",
	},
	giles2008: {
		kind: "paper",
		label: "Giles (2008)",
		cite: "Multilevel Monte Carlo path simulation. Operations Research 56(3), 607–617.",
		url: "https://doi.org/10.1287/opre.1070.0496",
	},
	gilesWaterhouse: {
		kind: "paper",
		label: "Giles & Waterhouse (2009)",
		cite: "Multilevel quasi-Monte Carlo path simulation. In Advanced Financial Modelling, Radon Series on Computational and Applied Mathematics 8, de Gruyter, 165–181.",
	},
	gilesMilstein: {
		kind: "paper",
		label: "Giles (2008b)",
		cite: "Improved multilevel Monte Carlo convergence using the Milstein scheme. In Monte Carlo and Quasi-Monte Carlo Methods 2006, Springer, 343–358.",
	},
	burgosGiles: {
		kind: "paper",
		label: "Burgos & Giles (2012)",
		cite: "Computing Greeks using multilevel path simulation. In Monte Carlo and Quasi-Monte Carlo Methods 2010, Springer, 281–296.",
	},
	talayTubaro: {
		kind: "paper",
		label: "Talay & Tubaro (1990)",
		cite: "Expansion of the global error for numerical schemes solving stochastic differential equations. Stochastic Analysis and Applications 8(4), 483–509.",
		url: "https://doi.org/10.1080/07362999008809220",
	},
	andersenQE: {
		kind: "paper",
		label: "Andersen (2008)",
		cite: "Simple and efficient simulation of the Heston stochastic volatility model. Journal of Computational Finance 11(3), 1–42.",
	},
	lord: {
		kind: "paper",
		label: "Lord, Koekkoek & van Dijk (2010)",
		cite: "A comparison of biased simulation schemes for stochastic volatility models. Quantitative Finance 10(2), 177–194.",
		url: "https://doi.org/10.1080/14697680802392496",
	},
	heston: {
		kind: "paper",
		label: "Heston (1993)",
		cite: "A closed-form solution for options with stochastic volatility with applications to bond and currency options. Review of Financial Studies 6(2), 327–343.",
		url: "https://doi.org/10.1093/rfs/6.2.327",
	},
	cos: {
		kind: "paper",
		label: "Fang & Oosterlee (2008)",
		cite: "A novel pricing method for European options based on Fourier-cosine series expansions. SIAM Journal on Scientific Computing 31(2), 826–848.",
		url: "https://doi.org/10.1137/080718061",
	},
	dupire: {
		kind: "paper",
		label: "Dupire (1994)",
		cite: "Pricing with a smile. Risk 7(1), 18–20.",
	},
	svi: {
		kind: "note",
		label: "Gatheral (2004)",
		cite: "A parsimonious arbitrage-free implied volatility parameterization with application to the valuation of volatility derivatives. Presentation, Global Derivatives & Risk Management, Madrid.",
	},
	gatheral: {
		kind: "book",
		label: "Gatheral (2006)",
		cite: "The Volatility Surface: A Practitioner's Guide, Wiley, ch. 1 (local volatility in terms of implied volatility).",
	},
	guyonHL: {
		kind: "paper",
		label: "Guyon & Henry-Labordère (2012)",
		cite: "Being particular about calibration. Risk, January 2012 (the particle method for stochastic-local volatility).",
	},
	hullWhite: {
		kind: "paper",
		label: "Hull & White (1990)",
		cite: "Pricing interest-rate-derivative securities. Review of Financial Studies 3(4), 573–592.",
		url: "https://doi.org/10.1093/rfs/3.4.573",
	},
	andersenPiterbarg: {
		kind: "book",
		label: "Andersen & Piterbarg (2010)",
		cite: "Interest Rate Modeling, Volume II: Term Structure Models, Atlantic Financial Press, ch. 10 (one-factor Gaussian short rate).",
	},
	jamshidian: {
		kind: "paper",
		label: "Jamshidian (1989)",
		cite: "An exact bond option formula. Journal of Finance 44(1), 205–209.",
		url: "https://doi.org/10.2307/2328284",
	},
	lsm: {
		kind: "paper",
		label: "Longstaff & Schwartz (2001)",
		cite: "Valuing American options by simulation: a simple least-squares approach. Review of Financial Studies 14(1), 113–147.",
		url: "https://doi.org/10.1093/rfs/14.1.113",
	},
	gilesGlasserman: {
		kind: "paper",
		label: "Giles & Glasserman (2006)",
		cite: "Smoking adjoints: fast Monte Carlo Greeks. Risk 19(1), 88–92.",
	},
	savine: {
		kind: "book",
		label: "Savine (2018)",
		cite: "Modern Computational Finance: AAD and Parallel Simulations, Wiley.",
	},
	andreasenSavine: {
		kind: "book",
		label: "Andreasen & Savine (2021)",
		cite: "Modern Computational Finance: Scripting for Derivatives and xVA, Wiley (the scripting language; fuzzy logic for discontinuous payoffs).",
	},
	vibrato: {
		kind: "paper",
		label: "Giles (2009)",
		cite: "Vibrato Monte Carlo sensitivities. In Monte Carlo and Quasi-Monte Carlo Methods 2008, Springer, 369–382.",
		url: "https://doi.org/10.1007/978-3-642-04107-5_23",
	},
	ghs: {
		kind: "paper",
		label: "Glasserman, Heidelberger & Shahabuddin (1999)",
		cite: "Asymptotically optimal importance sampling and stratification for pricing path-dependent options. Mathematical Finance 9(2), 117–152.",
		url: "https://doi.org/10.1111/1467-9965.00065",
	},
	bgk: {
		kind: "paper",
		label: "Broadie, Glasserman & Kou (1997)",
		cite: "A continuity correction for discrete barrier options. Mathematical Finance 7(4), 325–349.",
		url: "https://doi.org/10.1111/1467-9965.00035",
	},
	blackScholes: {
		kind: "paper",
		label: "Black & Scholes (1973)",
		cite: "The pricing of options and corporate liabilities. Journal of Political Economy 81(3), 637–654.",
		url: "https://doi.org/10.1086/260062",
	},
	glassermanPaths: {
		kind: "book",
		label: "Glasserman (2004a)",
		cite: "Monte Carlo Methods in Financial Engineering, Springer, ch. 2–3 (correlated Brownian motions by Cholesky; exact simulation of geometric Brownian motion and Gaussian short rates).",
	},
	glassermanVR: {
		kind: "book",
		label: "Glasserman (2004b)",
		cite: "Monte Carlo Methods in Financial Engineering, Springer, ch. 4 (antithetic variates, control variates, stratification).",
	},
	glassermanAmerican: {
		kind: "book",
		label: "Glasserman (2004c)",
		cite: "Monte Carlo Methods in Financial Engineering, Springer, ch. 8.6 (regression-based methods: a rule estimated on independent paths gives a low-biased price).",
	},
} satisfies Record<string, Reference>;

export type MethodRefKey = keyof typeof METHOD_REFS;
