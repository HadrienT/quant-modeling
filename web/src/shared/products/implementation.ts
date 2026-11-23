import MEASURED from "./methods.gen.json";
import { METHOD_REFS, type MethodRefKey } from "./methodRefs";
import type { Reference } from "./docs";
import type { ScriptedProduct } from "./types";

/**
 * The Products page's "How it is implemented" panel: everything a product's
 * price goes through on the site, from the model and its calibration to the
 * random numbers, the variance reduction and the measured cost of a price to
 * 0.1 %. The facts come from methods.gen.json, written by
 * scripts/bench_products.py from what the engine ran and reported (its
 * diagnostics); the prose below only names them and cites where each method
 * comes from, as `[@key]` (methodRefs.ts). Re-run the script when the engine
 * changes: the panel then follows.
 */

export type Measured = {
	slug: string;
	market: string;
	eps: number | null;
	greeks_smoothing: string | null;
	early_exercise: boolean;
	skipped?: string;
	error?: string;
	model?: string;
	model_reason?: string;
	device?: string;
	steps_per_year?: number | null;
	variance_scheme?: string | null;
	multilevel?: boolean;
	diagnostics?: string;
	npv?: number;
	std_error?: number | null;
	bias?: number | null;
	wall_seconds?: number;
	precision?: {
		target: number;
		stopped: string;
		paths: number | null;
		paths_needed: number | null;
		achieved_error: number;
		seconds: number | null;
		note: string;
	} | null;
};

export type Line = { label: string; text: string };
export type Section = { title: string; lines: Line[] };
/** A product the site cannot price: why, instead of a panel. */
export type Notice = { measuredAt: string; notice: string };
export type Implementation = {
	sections: Section[];
	/** cited references, in order of first citation: [1], [2], ... */
	refs: { key: MethodRefKey; ref: Reference }[];
	measuredAt: string;
	machine: string;
};

const DATA = MEASURED as {
	measured_at: string;
	machine: { cpu: string; gpus: string[]; commit: string };
	products: Measured[];
};
const BY_SLUG = new Map(DATA.products.map((m) => [m.slug, m]));

export const measuredOf = (slug: string) => BY_SLUG.get(slug);

const fmt = (x: number, digits = 3) =>
	Math.abs(x) >= 1e4
		? Math.round(x).toLocaleString("en-US")
		: Math.abs(x) < 1e-3 && x !== 0
			? x.toExponential(2)
			: +x.toPrecision(digits) + "";
const pctOf = (x: number, of: number) =>
	of ? `${+((100 * x) / Math.abs(of)).toPrecision(2)} %` : "—";
const capitalise = (t: string) => t.charAt(0).toUpperCase() + t.slice(1);
const grab = (s: string, re: RegExp) => s.match(re)?.[1];

const MODEL_NAMES: Record<string, string> = {
	black_scholes: "Black-Scholes",
	local_vol: "Local volatility",
	heston: "Heston",
	slv: "Stochastic-local volatility",
	hull_white: "Hull-White one-factor",
};

function modelLines(m: Measured, p: ScriptedProduct): Line[] {
	const multi = p.underlyings > 1;
	const calib: Record<string, string> = {
		local_vol: multi
			? "Each underlying on the Dupire surface [@dupire] of its own stored option chain, built from SVI slices [@svi; @gatheral]; the Brownian drivers correlated by the historical correlation of the stored closes, through a Cholesky factor [@glassermanPaths]."
			: `The Dupire surface [@dupire] derived from SVI slices [@svi] fitted to the stored option chain of ${m.market} [@gatheral]: every vanilla of that chain reprices.`,
		slv: "Heston [@heston] fitted to the SVI surface [@svi] by Levenberg-Marquardt on COS prices [@cos], times a leverage function calibrated by the particle method [@guyonHL] so that the model reprices the Dupire marginals [@dupire].",
		heston:
			"Heston [@heston] fitted to the SVI surface [@svi] by Levenberg-Marquardt on COS prices [@cos].",
		black_scholes:
			"A flat volatility: the at-the-money implied vol of the stored smile at the product's last date [@blackScholes].",
		hull_white:
			"$dx = (y(t) - a x)\\,dt + \\sigma\\,dW$ with $x = r - f(0,t)$ [@hullWhite; @andersenPiterbarg]: today's discount curve is fitted exactly, whatever $a$ and $\\sigma$.",
	};
	const lines: Line[] = [
		{
			label: "Model",
			text: `${MODEL_NAMES[m.model!] ?? m.model}, on ${m.market}.`,
		},
		{ label: "Why", text: m.model_reason ?? "" },
		{
			label: "Market inputs",
			text:
				p.market === "rates"
					? `The ${m.market} from the site's database (fed by the ingestion pipeline), read on the valuation date.`
					: `Spot, dividend yield and option chain of ${m.market} from the site's database, the latest stored snapshot; a flat rate of 4 % (the page's default, editable).`,
		},
	];
	const how = calib[m.model ?? ""];
	if (how)
		lines.push({
			label: m.model === "hull_white" ? "Dynamics" : "Calibration",
			text: how,
		});
	if (p.curve === "sofr")
		lines.push({
			label: "Swaptions",
			text: "$a$ and $\\sigma$ are calibrated to the at-the-money normal vols of the swaptions DTCC published, each priced in closed form by Jamshidian's decomposition [@jamshidian].",
		});
	return lines;
}

function pathLines(m: Measured): Line[] {
	const steps = m.steps_per_year;
	const diag = m.diagnostics ?? "";
	const scheme: Record<string, string> = {
		black_scholes:
			"Exact: the log-normal transition from one event date to the next, no time step [@glassermanPaths].",
		hull_white:
			"Exact: the Gaussian transition of $x$ from one event date to the next, discount factors in closed form [@andersenPiterbarg; @glassermanPaths].",
		local_vol: `Log-Euler at ${steps} steps a year, the local vol read off the grid at the start of each step. Milstein (strong order 1) is available [@gilesMilstein] but off: on a calibrated grid, steep at the edge of the quoted strikes, it couples the multilevel corrections worse.`,
		slv: `The variance by Andersen's quadratic-exponential scheme, martingale-corrected [@andersenQE]; the spot by log-Euler with the leverage held over each step; ${steps} steps a year (on SPY's fit, 64 a year put a one-year put 2 % above 504 a year, 252 within its noise).`,
		heston: `The variance by Andersen's quadratic-exponential scheme [@andersenQE], ${steps} steps a year and at least 32 over the product's life: nearly exact in law over a long step.`,
	};
	const events = grab(diag, /scripted, (\d+) events/);
	const lines: Line[] = [
		{ label: "Scheme", text: scheme[m.model!] ?? `${m.model}.` },
	];
	if (events)
		lines.push({
			label: "Event dates",
			text: `${events}: the dates the script reads${steps ? `; the time grid adds steps between them, never moves them` : ", the only dates simulated"}.`,
		});
	if (m.variance_scheme === "qe")
		lines.push({
			label: "For the greeks",
			text: "A second run steps the variance by full-truncation Euler [@lord]: QE's branches have no pathwise derivative.",
		});
	const why = grab(diag, /discretisation bias not estimated \(([^)]*)\)/);
	if (m.multilevel)
		lines.push({
			label: "Time-step bias",
			text: `Controlled by the multilevel engine (below): levels are added until the estimated remaining bias (here ${fmt(m.bias ?? 0)}) is under $\\varepsilon/\\sqrt2$ [@giles2008].`,
		});
	else if (/steps halved/.test(diag))
		lines.push({
			label: "Time-step bias",
			text: `Estimated by a second run with the steps halved, the bias read from the difference [@talayTubaro], and returned with the price (here ${fmt(m.bias ?? 0)}, ${pctOf(m.bias ?? 0, m.npv ?? 0)} of it). A precision target gives $\\varepsilon/\\sqrt2$ to the bias and $\\varepsilon/\\sqrt2$ to the standard error [@giles2008].`,
		});
	else if (why && m.model !== "black_scholes" && m.model !== "hull_white")
		lines.push({
			label: "Time-step bias",
			text: `Not estimated (${why}): the error below is the standard error alone.`,
		});
	return lines;
}

function samplingLines(m: Measured): Line[] {
	const diag = m.diagnostics ?? "";
	const dim = grab(diag, /dim=(\d+)/);
	const reps =
		grab(diag, /(\d+) replicates/) ?? grab(diag, /Sobol RQMC \((\d+) batches/);
	const lines: Line[] = [
		{
			label: "Points",
			text: `Sobol points [@sobol] with Joe-Kuo direction numbers [@joeKuo]${dim ? `, ${dim} dimensions` : ""}, mapped to normals by Wichura's AS 241 [@wichura].`,
		},
		{
			label: "Randomisation",
			text: `${reps ?? "16"} independent replicates, each shifted by its own random digital shift [@lecuyerLemieux]: the standard error is the spread of the replicate means, and the error still falls faster than $1/\\sqrt N$ on a smooth payoff.`,
		},
	];
	if (/Brownian bridge|multilevel QMC/.test(diag))
		lines.push({
			label: "Path construction",
			text: "The Brownian bridge [@moskowitzCaflisch]: the first Sobol coordinates set the path's end and midpoints, where the points are most uniform.",
		});
	return lines;
}

function varianceLines(m: Measured, p: ScriptedProduct): Line[] {
	const diag = m.diagnostics ?? "";
	const lines: Line[] = [];
	if (m.multilevel) {
		const levels = grab(diag, /levels (\d+\.\.\d+)/);
		const rates = grab(diag, /(alpha [\d.]+, beta [\d.]+, gamma [\d.]+)/);
		lines.push({
			label: "Multilevel",
			text: `Multilevel quasi-Monte Carlo over the time step [@giles2008; @gilesWaterhouse]: levels ${levels ?? "—"}, the fine and coarse grids of each level on the same Sobol points, points allocated where the variance per cost is largest, to a root-mean-square error of 0.1 % of the price${rates ? ` (measured rates: ${rates.replace(/(\d\.\d{2})\d+/g, "$1")})` : ""}.`,
		});
	} else {
		const no = grab(diag, /\(no multilevel Monte Carlo: ([^)]*\)?)\)/);
		const reason = m.early_exercise
			? "early exercise: each level would need its own exercise rule"
			: m.model === "slv" || m.model === "heston"
				? "under Heston and SLV the levels couple poorly (on SPY, 12 to 160 s on the CPU against about 2 s for the plain engine with its bias estimated)"
				: m.model === "black_scholes" || m.model === "hull_white"
					? "exact simulation: there is no time-step bias to remove"
					: (no ?? "the multilevel engine runs one underlying under local vol");
		lines.push({
			label: "Multilevel",
			text: `Not used: ${reason}. Plain randomised QMC [@giles2008].`,
		});
	}
	lines.push({
		label: "Antithetic",
		text: "Off with Sobol points (the mirror pairs belong to the pseudo-random sampler) [@glassermanVR].",
	});
	if (p.market !== "rates")
		lines.push({
			label: "Control variate",
			text: "Off by default. On request, the deflated spots at up to 8 event dates, martingales of known mean, are regressed out of the payoff [@glassermanVR].",
		});
	lines.push({
		label: "Importance sampling",
		text: "Off by default. On request, every Brownian factor shifted by the drift at the mode of payoff × density, each path weighted by its likelihood ratio, kept only when a pilot shows the variance falls [@ghs].",
	});
	return lines;
}

function productLines(m: Measured, p: ScriptedProduct): Line[] {
	const diag = m.diagnostics ?? "";
	const lines: Line[] = [];
	const lsm = diag.match(
		/LSMC exercise rule: (\d+) dates, (\d+) independent pilot paths, degree-(\d+) polynomial in (\d+) regressor/,
	);
	if (lsm)
		lines.push({
			label: "Early exercise",
			text: `Longstaff-Schwartz [@lsm]: on ${lsm[1]} exercise dates, the gain from exercising regressed on a degree-${lsm[3]} polynomial in ${lsm[4]} regressor(s), fitted on ${Number(lsm[2]).toLocaleString("en-US")} independent pilot paths (Philox [@salmon]), then applied to fresh pricing paths: the price is a lower bound, free of the foresight bias of reusing the regression's paths [@glassermanAmerican].`,
		});
	if (p.category === "Barriers and touch options")
		lines.push({
			label: "Monitoring",
			text: "On the closes the term sheet lists (discrete monitoring), so no continuity correction applies. A continuously monitored barrier would need the Broadie-Glasserman-Kou shift $e^{\\pm 0.5826\\,\\sigma\\sqrt{\\Delta t}}$ [@bgk].",
		});
	const greeks = [
		"By adjoint algorithmic differentiation [@gilesGlasserman; @savine]: every input's sensitivity from one adjoint run, on the GPU where the model has an adjoint kernel, else on the CPU tape, which is the oracle the GPU is tested against.",
	];
	if (m.multilevel)
		greeks.push(
			"On the GPU they come from the same levels, each level's fine-minus-coarse adjoint [@burgosGiles].",
		);
	if (m.eps != null)
		greeks.push(
			`The payoff jumps at a level: for the greeks its comparisons are smoothed over a half-width of ${m.eps}, measured for this product [@andreasenSavine]; ${
				m.greeks_smoothing === "joint"
					? "price and greeks come from one smoothed run (its price moves by less than half an error)"
					: "the price comes from an unsmoothed run, the greeks from a second, smoothed one (smoothing would move the price by more than half an error)"
			}. ${
				/digital/.test(m.slug)
					? "Second order (gamma, vanna, volga): Vibrato, pathwise up to the last step and a likelihood ratio over it [@vibrato]."
					: "Second order: not covered by Vibrato [@vibrato], which needs the jump at the last date; the page says so in its diagnostics."
			}`,
		);
	lines.push({ label: "Greeks", text: greeks.join(" ") });
	const gpus = grab(diag, /(\d+ x Tesla[^,)]*|Tesla[^,)]*)/);
	const threads = grab(diag, /on (\d+ threads)/);
	const cpuWhy =
		grab(diag, /not on the GPU: ([^)]*)\)/) ??
		grab(diag, /\(on the CPU: (.*?\))\)/);
	const where =
		m.device === "gpu"
			? `On the GPU (${gpus ?? DATA.machine.gpus.join(", ")}): the script compiled to a CUDA kernel at run time (NVRTC), its terms read as data, so one kernel serves every term sheet.`
			: `On the CPU${threads ? `, ${threads}` : ""}${cpuWhy ? `, not on the GPU: ${cpuWhy}` : ""}.`;
	lines.push({ label: "Where it runs", text: where });
	return lines;
}

const OUTCOME: Record<string, (m: Measured) => string> = {
	target_reached: () => "reached",
	multilevel_target: () => "reached by the multilevel engine",
	multilevel_short: () => "not reached by the multilevel engine",
	bias_floor: (m) =>
		`not reachable at ${m.steps_per_year} steps a year: the time-step bias is above target / √2, which finer steps or multilevel would lower, not more paths`,
	out_of_budget: () =>
		"the pilot predicted more paths than the 5,000,000 cap; the run used the cap",
	time_budget: () => "the 60 s budget ran out first",
};

function performanceLines(m: Measured): Line[] {
	const pr = m.precision;
	if (!pr || m.npv == null) return [];
	const lines: Line[] = [
		{
			label: "Target",
			text: `0.1 % of the price, ${fmt(pr.target)}: a root-mean-square error, the time-step bias included where it is estimated.`,
		},
		{
			label: "Outcome",
			text: capitalise((OUTCOME[pr.stopped] ?? (() => pr.stopped))(m)) + ".",
		},
		{
			label: "Error achieved",
			text: `${fmt(pr.achieved_error)} (${pctOf(pr.achieved_error, m.npv)} of the price ${fmt(m.npv, 6)}); standard error ${fmt(m.std_error ?? 0)}${m.bias != null ? `, time-step bias ${fmt(m.bias)}` : ""}.`,
		},
		{
			label: "Cost",
			text: `${pr.paths != null ? `${pr.paths.toLocaleString("en-US")} paths${m.multilevel ? " over all levels" : ""} in ` : ""}${fmt(m.wall_seconds ?? 0)} s, the price only, the market and calibration already cached as on a running server.`,
		},
	];
	return lines;
}

/** The panel of a library product, or undefined when nothing was measured. */
export function implementationOf(
	p: ScriptedProduct,
): Implementation | Notice | undefined {
	const m = measuredOf(p.slug);
	if (!m) return undefined;
	if (m.skipped || m.error)
		return { measuredAt: DATA.measured_at, notice: m.skipped ?? m.error! };
	const sections: Section[] = [
		{ title: "Model and calibration", lines: modelLines(m, p) },
		{ title: "Path simulation", lines: pathLines(m) },
		{ title: "Random numbers", lines: samplingLines(m) },
		{ title: "Variance reduction", lines: varianceLines(m, p) },
		{ title: "Product features and risks", lines: productLines(m, p) },
		{ title: "Measured performance", lines: performanceLines(m) },
	].filter((s) => s.lines.length > 0);
	return {
		sections,
		refs: citedRefs(sections),
		measuredAt: DATA.measured_at,
		machine: `${DATA.machine.gpus.length ? `${DATA.machine.gpus.length} × ${DATA.machine.gpus[0]}, ` : ""}${DATA.machine.cpu}, commit ${DATA.machine.commit}`,
	};
}

export const CITE = /\[@([\w;\s@]+)\]/g;

/** Every key cited, in order of first appearance. */
export function citedRefs(sections: Section[]) {
	const order: MethodRefKey[] = [];
	for (const s of sections)
		for (const l of s.lines)
			for (const match of l.text.matchAll(CITE))
				for (const k of (match[1] ?? "").split(/;\s*@?/)) {
					const key = k.replace("@", "").trim() as MethodRefKey;
					if (!order.includes(key)) order.push(key);
				}
	return order.map((key) => ({ key, ref: METHOD_REFS[key] }));
}

/** The panel's closed summary: model, estimator, device, measured cost. */
export function headlineOf(slug: string): string[] {
	const m = measuredOf(slug);
	if (!m?.model) return [];
	const pr = m.precision;
	const reached =
		pr &&
		(pr.stopped === "target_reached" ||
			pr.stopped === "multilevel_target" ||
			pr.achieved_error <= pr.target);
	return [
		MODEL_NAMES[m.model] ?? m.model,
		m.multilevel ? "Multilevel QMC" : "Randomised QMC",
		m.device === "gpu" ? "GPU" : "CPU",
		...(pr
			? [
					reached
						? `0.1 % in ${fmt(m.wall_seconds ?? 0, 2)} s`
						: `0.1 % not reached (${pctOf(pr.achieved_error, m.npv ?? 0)})`,
				]
			: []),
	];
}
