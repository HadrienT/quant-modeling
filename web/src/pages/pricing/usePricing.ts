import { useCallback, useMemo, useRef, useState } from "react";
import type {
	AverageType,
	BarrierKind,
	BondCurveSource,
	BondType,
	CategoryType,
	CommodityProductType,
	CouponFrequency,
	CurvePoint,
	DigitalPayoffType,
	EngineType,
	ExoticProductType,
	FXProductType,
	InstrumentType,
	LookbackExtremum,
	LookbackStyle,
	ModelType,
	PricingResponse,
	ProductType,
	RainbowKind,
	StructuredProductType,
	VolProductType,
	VolSourceType,
} from "./types";

const REQUEST_TIMEOUT_MS = 20_000;

const BOND_FREQ_MAP: Record<CouponFrequency, number> = {
	annual: 1,
	semiannual: 2,
	quarterly: 4,
};

const SAMPLE_CURVE: CurvePoint[] = [
	{ time: 0.25, df: 0.992 },
	{ time: 0.5, df: 0.985 },
	{ time: 1, df: 0.97 },
	{ time: 2, df: 0.945 },
	{ time: 3, df: 0.92 },
	{ time: 5, df: 0.88 },
];

export function usePricing() {
	const [category, setCategoryRaw] = useState<CategoryType>("vanilla");
	const [product, setProduct] = useState<ProductType>("vanilla");
	const [instrument, setInstrument] = useState<InstrumentType>("option");
	const [engine, setEngine] = useState<EngineType>("analytic");

	/* ── model selection state ─────────────────────── */
	const [model, setModel] = useState<ModelType>("black-scholes");
	const [volSource, setVolSource] = useState<VolSourceType>("manual");
	const [ticker, setTicker] = useState("");
	const [tickerLoading, setTickerLoading] = useState(false);
	const [tickerError, setTickerError] = useState<string | null>(null);
	const [lvNStepsPerYear, setLvNStepsPerYear] = useState(252);

	/* ── exotic state ─────────────────────────────── */
	const [exoticProduct, setExoticProduct] = useState<ExoticProductType>("barrier");
	const [barrierLevel, setBarrierLevel] = useState(120);
	const [barrierKind, setBarrierKind] = useState<BarrierKind>("up-and-out");
	const [rebate, setRebate] = useState(0);
	const [brownianBridge, setBrownianBridge] = useState(true);
	const [digitalPayoff, setDigitalPayoff] = useState<DigitalPayoffType>("cash-or-nothing");
	const [cashAmount, setCashAmount] = useState(1);
	/* lookback */
	const [lookbackStyle, setLookbackStyle] = useState<LookbackStyle>("fixed-strike");
	const [lookbackExtremum, setLookbackExtremum] = useState<LookbackExtremum>("maximum");
	const [lookbackNSteps, setLookbackNSteps] = useState(0);
	const [basketWeights, setBasketWeights] = useState("0.5,0.5");
	const [basketSpots, setBasketSpots] = useState("100,120");
	const [basketVols, setBasketVols] = useState("0.2,0.25");
	const [basketDividends, setBasketDividends] = useState("0,0");
	const [basketCorrelation, setBasketCorrelation] = useState(0.5);
	const [averageType, setAverageType] = useState<AverageType>("arithmetic");

	/* ── rainbow state ───────────────────────────── */
	const [rainbowKind, setRainbowKind] = useState<RainbowKind>("worst-of");
	const [rainbowSpots, setRainbowSpots] = useState("100,120");
	const [rainbowVols, setRainbowVols] = useState("0.2,0.25");
	const [rainbowDividends, setRainbowDividends] = useState("0,0");
	const [rainbowCorrelation, setRainbowCorrelation] = useState(0.5);

	/* ── structured state ────────────────────────── */
	const [structuredProduct, setStructuredProduct] = useState<StructuredProductType>("autocall");
	/* autocall */
	const [autocallBarrier, setAutocallBarrier] = useState(1.0);
	const [couponBarrier, setCouponBarrier] = useState(0.8);
	const [putBarrier, setPutBarrier] = useState(0.6);
	const [autocallCouponRate, setAutocallCouponRate] = useState(0.05);
	const [memoryCoupon, setMemoryCoupon] = useState(true);
	const [kiContinuous, setKiContinuous] = useState(false);
	const [observationDates, setObservationDates] = useState("0.25,0.5,0.75,1.0");
	/* mountain */
	const [mountainSpots, setMountainSpots] = useState("100,110,105");
	const [mountainVols, setMountainVols] = useState("0.2,0.25,0.22");
	const [mountainDividends, setMountainDividends] = useState("0,0,0");
	const [mountainCorrelations, setMountainCorrelations] = useState("");
	const [mountainObsDates, setMountainObsDates] = useState("0.25,0.5,0.75,1.0");

	/* ── volatility product state ────────────────── */
	const [volProduct, setVolProduct] = useState<VolProductType>("variance-swap");
	const [strikeVar, setStrikeVar] = useState(0.04);
	const [strikeVol, setStrikeVol] = useState(0.2);
	const [varSwapEngine, setVarSwapEngine] = useState<"analytic" | "mc">("analytic");
	const [volObsDates, setVolObsDates] = useState("");
	/* dispersion */
	const [dispersionSpots, setDispersionSpots] = useState("100,120");
	const [dispersionVols, setDispersionVols] = useState("0.2,0.25");
	const [dispersionDividends, setDispersionDividends] = useState("0,0");
	const [dispersionWeights, setDispersionWeights] = useState("0.5,0.5");
	const [dispersionCorrelation, setDispersionCorrelation] = useState(0.5);
	const [strikeSpread, setStrikeSpread] = useState(0.0);

	/* ── FX product state ───────────────────────── */
	const [fxProduct, setFxProduct] = useState<FXProductType>("fx-forward");
	const [rateDomestic, setRateDomestic] = useState(0.05);
	const [rateForeign, setRateForeign] = useState(0.02);

	/* ── Commodity product state ────────────────── */
	const [commodityProduct, setCommodityProduct] = useState<CommodityProductType>("commodity-forward");
	const [storageCost, setStorageCost] = useState(0.0);
	const [convenienceYield, setConvenienceYield] = useState(0.0);
	const [bondType, setBondType] = useState<BondType>("fixed-rate");
	const [isCall, setIsCall] = useState(true);
	const [spot, setSpot] = useState(100);
	const [strike, setStrike] = useState(100);
	const [maturity, setMaturity] = useState(1);
	const [rate, setRate] = useState(0.02);
	const [dividend, setDividend] = useState(0.0);
	const [vol, setVol] = useState(0.2);
	const [notional, setNotional] = useState(1);
	const [faceValue, setFaceValue] = useState(1000);
	const [couponRate, setCouponRate] = useState(0.05);
	const [yieldRate, setYieldRate] = useState(0.04);
	const [couponFrequency, setCouponFrequency] = useState<CouponFrequency>("annual");
	const [useCurve, setUseCurve] = useState(true);
	const [bondCurveSource, setBondCurveSource] = useState<BondCurveSource>("Treasury");
	const [curvePoints, setCurvePoints] = useState<CurvePoint[]>([
		{ time: 0.5, df: 0.985 },
		{ time: 1, df: 0.97 },
		{ time: 2, df: 0.94 },
		{ time: 3, df: 0.91 },
		{ time: 5, df: 0.86 },
	]);
	const [curveLoading, setCurveLoading] = useState(false);
	const [nPaths, setNPaths] = useState(200000);
	const [seed, setSeed] = useState(1);
	const [mcEpsilon, setMcEpsilon] = useState(0.0);
	const [treeSteps, setTreeSteps] = useState(100);
	const [pdeSpaceSteps, setPdeSpaceSteps] = useState(100);
	const [pdeTimeSteps, setPdeTimeSteps] = useState(100);
	const [loading, setLoading] = useState(false);
	const [error, setError] = useState<string | null>(null);
	const [result, setResult] = useState<PricingResponse | null>(null);
	const abortControllerRef = useRef<AbortController | null>(null);

	const setCategory = useCallback((c: CategoryType) => {
		setCategoryRaw(c);
		setResult(null);
		setError(null);
		setModel("black-scholes");
		setVolSource("manual");
	}, []);

	/* Reset model to BS when an analytic engine is selected */
	const setEngineWrapped = useCallback((e: EngineType) => {
		setEngine(e);
		if (e === "analytic") {
			setModel("black-scholes");
			setVolSource("manual");
		}
	}, []);

	/* ── derived: is Dupire model with live ticker? ── */
	const isDupire = model === "dupire-local-vol";
	const isLiveVol = isDupire && volSource === "real";

	const fairForward = useMemo(
		() => spot * Math.exp((rate - dividend) * maturity),
		[spot, rate, dividend, maturity],
	);

	const apiBase = useMemo(() => import.meta.env.VITE_API_BASE ?? "/api", []);

	/* ── load a market rate curve → discount factors ── */

	const loadMarketCurve = useCallback(async (curveName: string) => {
		setCurveLoading(true);
		try {
			const apiKey = import.meta.env.VITE_API_KEY;
			const headers: Record<string, string> = { "Content-Type": "application/json" };
			if (apiKey) headers["X-API-KEY"] = apiKey;

			const res = await fetch(
				`${apiBase}/market/rates/curve?curve=${encodeURIComponent(curveName)}&curve_type=zero`,
				{ headers },
			);
			if (!res.ok) throw new Error(`Failed to fetch ${curveName} curve`);

			const data = (await res.json()) as { zero: { x: number; y: number }[] };
			const points: CurvePoint[] = data.zero.map((p) => ({
				time: p.x,
				df: Math.exp(-(p.y / 100) * p.x),   // continuous DF from yield proxy
			}));
			if (points.length > 0) {
				setCurvePoints(points);
				setUseCurve(true);
			}
		} catch (err) {
			setError(err instanceof Error ? err.message : "Curve fetch failed");
		} finally {
			setCurveLoading(false);
		}
	}, [apiBase]);

	/* ── helpers ───────────────────────────────────── */

	const formatNumber = (value?: number | null) =>
		value === null || value === undefined ? "-" : value.toFixed(3);

	const formatWithError = (value?: number | null, errorValue?: number | null) => {
		const formatted = formatNumber(value);
		if (formatted === "-") return "-";
		const errorFormatted = formatNumber(errorValue);
		if (errorFormatted === "-") return formatted;
		return `${formatted} +/- ${errorFormatted}`;
	};

	const buildCurvePayload = () => {
		if (!useCurve) return { discount_times: [] as number[], discount_factors: [] as number[] };
		const cleaned = curvePoints
			.map((p) => ({ time: Number(p.time), df: Number(p.df) }))
			.filter((p) => Number.isFinite(p.time) && Number.isFinite(p.df) && p.time > 0 && p.df > 0)
			.sort((a, b) => a.time - b.time);
		return {
			discount_times: cleaned.map((p) => p.time),
			discount_factors: cleaned.map((p) => p.df),
		};
	};

	/* ── actions ───────────────────────────────────── */

	const stopPricing = () => {
		if (abortControllerRef.current) {
			abortControllerRef.current.abort();
			abortControllerRef.current = null;
		}
		setLoading(false);
		setError("Pricing cancelled.");
	};

	const handleSubmit = async (event: React.FormEvent<HTMLFormElement>) => {
		event.preventDefault();
		setLoading(true);
		setError(null);
		setResult(null);

		const controller = new AbortController();
		abortControllerRef.current = controller;
		let timedOut = false;
		const timeoutId = window.setTimeout(() => {
			timedOut = true;
			controller.abort();
		}, REQUEST_TIMEOUT_MS);

		try {
			const curvePayload = buildCurvePayload();
			let endpoint = "/price/option/vanilla";
			let payload: Record<string, unknown> = {};
			let method: "GET" | "POST" = "POST";

			/* ── Dupire local-vol with live ticker ───────────── */
			if (isDupire && isLiveVol) {
				if (!ticker.trim()) throw new Error("Ticker is required for real-data local vol.");
				method = "GET";
				const params = new URLSearchParams({
					ticker: ticker.trim().toUpperCase(),
					strike: String(strike),
					maturity: String(maturity),
					is_call: String(isCall),
					rate: String(rate),
					n_paths: String(nPaths),
					n_steps_per_year: String(lvNStepsPerYear),
					seed: String(seed),
					compute_greeks: "true",
				});
				endpoint = `/local-vol/price?${params.toString()}`;
			/* ── Bond pricing ───────────────────────────────── */
			} else if (category === "fixed-income") {
				endpoint = bondType === "zero-coupon" ? "/price/bond/zero-coupon" : "/price/bond/fixed-rate";
				payload = { maturity, rate: yieldRate, notional: faceValue, ...curvePayload };
				if (bondType === "fixed-rate") {
					payload.coupon_rate = couponRate;
					payload.coupon_frequency = BOND_FREQ_MAP[couponFrequency];
				}
			} else if (category === "exotics") {
				/* Exotic options — endpoint per product, payload varies */
				const base = {
					spot, strike, maturity, rate, dividend, vol,
					is_call: isCall,
					engine: "mc" as const,
					n_paths: nPaths,
					seed,
					mc_epsilon: mcEpsilon,
				};
				switch (exoticProduct) {
					case "barrier":
						endpoint = "/price/option/barrier";
						payload = { ...base, barrier_level: barrierLevel, barrier_kind: barrierKind, rebate, brownian_bridge: brownianBridge };
						break;
					case "digital":
						endpoint = "/price/option/digital";
						payload = { ...base, payoff_type: digitalPayoff, cash_amount: cashAmount };
						break;
					case "lookback":
						endpoint = "/price/option/lookback";
					payload = {
						...base,
						style: lookbackStyle,
						extremum: lookbackExtremum,
						n_steps: lookbackNSteps,
						mc_antithetic: true,
					};
						break;
					case "basket":
						endpoint = "/price/option/basket";
						payload = {
							spot, strike, maturity, rate,
							is_call: isCall,
							n_paths: nPaths,
							seed,
							spots: basketSpots.split(",").map(Number),
							vols: basketVols.split(",").map(Number),
							dividends: basketDividends.split(",").map(Number),
							weights: basketWeights.split(",").map(Number),
							pairwise_correlation: basketCorrelation,
							mc_antithetic: true,
						};
						break;
					case "rainbow":
						endpoint = "/price/option/rainbow";
						payload = {
							spots: rainbowSpots.split(",").map(Number),
							vols: rainbowVols.split(",").map(Number),
							dividends: rainbowDividends.split(",").map(Number),
							pairwise_correlation: rainbowCorrelation,
							maturity, strike, is_call: isCall, rate,
							notional, rainbow_kind: rainbowKind,
							n_paths: nPaths, seed,
						};
						break;
				}
			} else if (category === "vanilla" && instrument === "future") {
				endpoint = "/price/future";
				payload = { spot, strike, maturity, rate, dividend, notional };

			/* ── Structured products ─────────────────────── */
			} else if (category === "structured") {
				if (structuredProduct === "autocall") {
					endpoint = "/price/structured/autocall";
					payload = {
						spot, rate, dividend, vol,
						observation_dates: observationDates.split(",").map(Number),
						autocall_barrier: autocallBarrier,
						coupon_barrier: couponBarrier,
						put_barrier: putBarrier,
						coupon_rate: autocallCouponRate,
						notional, memory_coupon: memoryCoupon,
						ki_continuous: kiContinuous,
						n_paths: nPaths, seed,
					};
				} else {
					endpoint = "/price/structured/mountain";
					const mSpots = mountainSpots.split(",").map(Number);
					const mVols = mountainVols.split(",").map(Number);
					const mDivs = mountainDividends.split(",").map(Number);
					let correlations: number[][] = [];
					if (mountainCorrelations.trim()) {
						const flat = mountainCorrelations.split(",").map(Number);
						const n = mSpots.length;
						correlations = Array.from({ length: n }, (_, i) =>
							Array.from({ length: n }, (_, j) => flat[i * n + j] ?? (i === j ? 1 : 0)),
						);
					}
					payload = {
						spots: mSpots, vols: mVols, dividends: mDivs,
						correlations,
						observation_dates: mountainObsDates.split(",").map(Number),
						strike, is_call: isCall, rate, notional,
						n_paths: nPaths, seed,
					};
				}

			/* ── Volatility products ─────────────────────── */
			} else if (category === "volatility") {
				const obsDates = volObsDates.trim() ? volObsDates.split(",").map(Number) : [];
				if (volProduct === "variance-swap") {
					endpoint = "/price/volatility/variance-swap";
					payload = {
						spot, rate, dividend, vol, maturity,
						strike_var: strikeVar, notional,
						observation_dates: obsDates,
						engine: varSwapEngine,
						n_paths: nPaths, seed,
					};
				} else if (volProduct === "volatility-swap") {
					endpoint = "/price/volatility/volatility-swap";
					payload = {
						spot, rate, dividend, vol, maturity,
						strike_vol: strikeVol, notional,
						observation_dates: obsDates,
						n_paths: nPaths, seed,
					};
				} else {
					endpoint = "/price/volatility/dispersion-swap";
					payload = {
						spots: dispersionSpots.split(",").map(Number),
						vols: dispersionVols.split(",").map(Number),
						dividends: dispersionDividends.split(",").map(Number),
						weights: dispersionWeights.split(",").map(Number),
						pairwise_correlation: dispersionCorrelation,
						maturity, strike_spread: strikeSpread,
						rate, notional,
						observation_dates: obsDates,
						n_paths: nPaths, seed,
					};
				}

			/* ── FX products ─────────────────────────────── */
			} else if (category === "fx") {
				if (fxProduct === "fx-forward") {
					endpoint = "/price/fx/forward";
					payload = {
						spot, rate_domestic: rateDomestic, rate_foreign: rateForeign,
						vol, strike, maturity, notional,
					};
				} else {
					endpoint = "/price/fx/option";
					payload = {
						spot, rate_domestic: rateDomestic, rate_foreign: rateForeign,
						vol, strike, maturity, is_call: isCall, notional,
					};
				}

			/* ── Commodity products ──────────────────────── */
			} else if (category === "commodity") {
				if (commodityProduct === "commodity-forward") {
					endpoint = "/price/commodity/forward";
					payload = {
						spot, rate, storage_cost: storageCost,
						convenience_yield: convenienceYield,
						vol, strike, maturity, notional,
					};
				} else {
					endpoint = "/price/commodity/option";
					payload = {
						spot, rate, storage_cost: storageCost,
						convenience_yield: convenienceYield,
						vol, strike, maturity, is_call: isCall, notional,
					};
				}

			} else {
				/* Vanilla options (european, american, asian) */
				endpoint = "/price/option/vanilla";
				payload = {
					spot, strike, maturity, rate, dividend, vol,
					is_call: isCall,
					is_american: product === "american",
					engine,
					n_paths: nPaths,
					seed,
					mc_epsilon: mcEpsilon,
					tree_steps: treeSteps,
					pde_space_steps: pdeSpaceSteps,
					pde_time_steps: pdeTimeSteps,
				};
				if (product === "asian") {
					endpoint = "/price/option/asian";
					delete payload.is_american;
					delete payload.tree_steps;
					payload.average_type = averageType;
				}
			}

			const apiKey = import.meta.env.VITE_API_KEY;
			const headers: Record<string, string> = { "Content-Type": "application/json" };
			if (apiKey) headers["X-API-KEY"] = apiKey;

			const fetchOpts: RequestInit = {
				method,
				headers,
				signal: controller.signal,
			};
			if (method === "POST") {
				fetchOpts.body = JSON.stringify(payload);
			}

			const response = await fetch(`${apiBase}${endpoint}`, fetchOpts);

			if (!response.ok) {
				const text = await response.text();
				throw new Error(text || "Pricing request failed.");
			}

			const raw = await response.json();

			/* Normalize local-vol response to PricingResponse shape */
			let data: PricingResponse;
			if (isDupire && isLiveVol && raw.npv !== undefined && !raw.greeks) {
				data = {
					npv: raw.npv,
					greeks: {
						delta: raw.delta ?? null,
						gamma: raw.gamma ?? null,
						vega: raw.vega_parallel ?? null,
						theta: raw.theta ?? null,
						rho: raw.rho ?? null,
					},
					mc_std_error: raw.mc_std_error ?? null,
					diagnostics: raw.diagnostics ?? `${raw.n_clean_quotes} clean quotes · ${raw.cleaning_summary ?? ""}`,
				};
			} else {
				data = raw as PricingResponse;
			}
			setResult(data);
		} catch (err) {
			const aborted = err instanceof DOMException && err.name === "AbortError";
			const message = aborted
				? timedOut
					? "Pricing timed out after 20 seconds. Try smaller grid/path settings."
					: "Pricing cancelled."
				: err instanceof Error
					? err.message
					: "Unexpected error.";
			setError(message);
		} finally {
			window.clearTimeout(timeoutId);
			if (abortControllerRef.current === controller) abortControllerRef.current = null;
			setLoading(false);
		}
	};

	return {
		/* category */
		category, setCategory,
		/* model selection */
		model, setModel,
		volSource, setVolSource,
		ticker, setTicker,
		tickerLoading, tickerError,
		isDupire, isLiveVol,
		lvNStepsPerYear, setLvNStepsPerYear,
		/* instrument / product */
		instrument, setInstrument,
		product, setProduct,
		engine, setEngine: setEngineWrapped,
		isCall, setIsCall,
		averageType, setAverageType,
		/* exotic fields */
		exoticProduct, setExoticProduct,
		barrierLevel, setBarrierLevel,
		barrierKind, setBarrierKind,
		rebate, setRebate,
		brownianBridge, setBrownianBridge,
		digitalPayoff, setDigitalPayoff,
		cashAmount, setCashAmount,
		/* lookback */
		lookbackStyle, setLookbackStyle,
		lookbackExtremum, setLookbackExtremum,
		lookbackNSteps, setLookbackNSteps,
		basketWeights, setBasketWeights,
		basketSpots, setBasketSpots,
		basketVols, setBasketVols,
		basketDividends, setBasketDividends,
		basketCorrelation, setBasketCorrelation,
		/* rainbow */
		rainbowKind, setRainbowKind,
		rainbowSpots, setRainbowSpots,
		rainbowVols, setRainbowVols,
		rainbowDividends, setRainbowDividends,
		rainbowCorrelation, setRainbowCorrelation,
		/* structured */
		structuredProduct, setStructuredProduct,
		autocallBarrier, setAutocallBarrier,
		couponBarrier, setCouponBarrier,
		putBarrier, setPutBarrier,
		autocallCouponRate, setAutocallCouponRate,
		memoryCoupon, setMemoryCoupon,
		kiContinuous, setKiContinuous,
		observationDates, setObservationDates,
		mountainSpots, setMountainSpots,
		mountainVols, setMountainVols,
		mountainDividends, setMountainDividends,
		mountainCorrelations, setMountainCorrelations,
		mountainObsDates, setMountainObsDates,
		/* volatility products */
		volProduct, setVolProduct,
		strikeVar, setStrikeVar,
		strikeVol, setStrikeVol,
		varSwapEngine, setVarSwapEngine,
		volObsDates, setVolObsDates,
		dispersionSpots, setDispersionSpots,
		dispersionVols, setDispersionVols,
		dispersionDividends, setDispersionDividends,
		dispersionWeights, setDispersionWeights,
		dispersionCorrelation, setDispersionCorrelation,
		strikeSpread, setStrikeSpread,
		/* FX */
		fxProduct, setFxProduct,
		rateDomestic, setRateDomestic,
		rateForeign, setRateForeign,
		/* commodity */
		commodityProduct, setCommodityProduct,
		storageCost, setStorageCost,
		convenienceYield, setConvenienceYield,
		/* option / future fields */
		spot, setSpot,
		strike, setStrike,
		maturity, setMaturity,
		rate, setRate,
		dividend, setDividend,
		vol, setVol,
		notional, setNotional,
		/* MC / tree / PDE params */
		nPaths, setNPaths,
		seed, setSeed,
		mcEpsilon, setMcEpsilon,
		treeSteps, setTreeSteps,
		pdeSpaceSteps, setPdeSpaceSteps,
		pdeTimeSteps, setPdeTimeSteps,
		/* bond fields */
		bondType, setBondType,
		faceValue, setFaceValue,
		couponRate, setCouponRate,
		yieldRate, setYieldRate,
		couponFrequency, setCouponFrequency,
		useCurve, setUseCurve,
		bondCurveSource, setBondCurveSource,
		curvePoints, setCurvePoints,
		sampleCurve: SAMPLE_CURVE,
		loadMarketCurve,
		curveLoading,
		/* derived / results */
		fairForward,
		loading, error, result,
		/* actions */
		handleSubmit, stopPricing,
		formatNumber, formatWithError,
	};
}

export type PricingHook = ReturnType<typeof usePricing>;
