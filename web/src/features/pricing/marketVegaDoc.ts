/** One sentence under the button: what the computation is for. */
export const MARKET_VEGA_LEAD =
	"Breaks the product's vega down by listed option: for each quoted implied vol of the ticker's chain, how much the price moves if that one quote rises by 1 vol point. It shows which listed options carry the product's volatility risk, hence which ones would hedge it.";

/** The explanation of the vega by listed option, opened on demand. */
export const MARKET_VEGA_DOC: { title: string; paragraphs: string[] }[] = [
	{
		title: "What it answers",
		paragraphs: [
			"The Vega of the results panel is one number: the price change when every volatility moves up together. A real volatility surface never moves that way; short and long maturities, low and high strikes move separately.",
			"This computation gives one vega per listed option instead. Each number is the price change for +1 vol point (0.20 to 0.21) on that single quote, the rest of the chain unchanged. Their sum is the parallel vega shown at the top of the result.",
		],
	},
	{
		title: "How to read the surface",
		paragraphs: [
			"One axis is the maturity of the listed option, the other its strike as log-moneyness ln(K/F), 0 being at the forward. The height and colour of a cell are the summed vegas of the quotes that fall in it; a cell with no listed option is left empty.",
			"A mostly flat surface is the expected result, not a failure: a product depends on a small part of the chain. A one-year call only reacts to the quotes near its strike on the listed maturities around its expiry, and to nothing that expires later. A barrier adds a second zone near the barrier level. The few cells that stand out are the answer; the flat part says the product is indifferent to those options.",
			"A positive value means the product gains when that option's implied vol rises, a negative one that it loses.",
		],
	},
	{
		title: "How it is computed",
		paragraphs: [
			"The product is priced by Monte-Carlo under a local volatility calibrated on the stored chain: an SVI smile is fitted to each listed maturity, and the Dupire formula turns these smiles into the local volatility grid.",
			"AAD (adjoint algorithmic differentiation) then runs the computation backwards once and returns the sensitivity of the price to every point of that grid, where bumping each point would take one full re-pricing per point. The same backward pass continues through the Dupire formula and through each SVI fit, down to the quoted implied vols.",
		],
	},
	{
		title: "What to trust",
		paragraphs: [
			"The ± is the Monte-Carlo standard error, measured on 8 independent batches. The parallel vega and the totals by maturity are the robust figures. A single quote is more fragile: moving it refits the whole smile of its maturity, so two neighbouring quotes can show opposite signs that cancel in the total.",
		],
	},
	{
		title: "Sources",
		paragraphs: [
			"Savine, Modern Computational Finance: AAD and Parallel Simulations, Wiley, 2018, ch. 13 (check-pointing and calibration; the Dupire superbucket).",
			"Dupire, “Pricing with a Smile”, Risk 7(1), 18–20, 1994.",
			"Gatheral & Jacquier, “Arbitrage-free SVI volatility surfaces”, Quantitative Finance 14(1), 59–71, 2014.",
		],
	},
];
