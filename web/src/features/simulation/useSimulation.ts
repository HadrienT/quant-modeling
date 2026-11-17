import { useEffect, useState } from "react";
import {
	useCalibrateSimulation,
	useSimulationDistribution,
} from "@/shared/api";
import type {
	DistributionRequest,
	SimulationCalibrateResponse,
	SimulationModel,
} from "@/shared/api/types";
import { useMarketContext } from "@/shared/session";
import { useHestonParams } from "./useHestonParams";
import { usePathReveal } from "./usePathReveal";

/**
 * All state and side effects for the simulation page: model/mode selection,
 * every parameter field (shared, BS-specific, SABR-specific), the calibrate-
 * on-ticker flow, the distribution request (WP 29: the law of the paths
 * and the prices they imply), the date under the cursor, and the client-side
 * "drawing" animation over an already-fetched batch of paths. Kept out of
 * SimulationPage.tsx to stay under this project's per-feature-file line
 * budget (tests/discipline.test.ts) — same rationale as pricing's
 * useWorkbench.ts.
 */
/** The page's up-and-out call: struck at the spot, knocked out 20% above. */
const BARRIER = 1.2;
const BARRIER_STRIKE = 1.0;

export function useSimulation() {
	const [model, setModel] = useState<SimulationModel>("black_scholes");
	const [mode, setMode] = useState<"manual" | "calibrate">("manual");

	// Shared
	const [ttm, setTtm] = useState("1.0");
	const [nSteps, setNSteps] = useState("60");
	const [nPaths, setNPaths] = useState("10000");
	const [seed, setSeed] = useState("1");

	// Black-Scholes params
	const [spot, setSpot] = useState("100");
	const [rate, setRate] = useState("5");
	const [dividend, setDividend] = useState("0");
	const [vol, setVol] = useState("20");

	// SABR params
	const [forward, setForward] = useState("100");
	const [alpha, setAlpha] = useState("0.2");
	const [beta, setBeta] = useState("0.5");
	const [rho, setRho] = useState("-0.4");
	const [nu, setNu] = useState("0.5");

	// Calibrate-on-ticker mode
	// Starts on the underlying last chosen on Market or Credit (WP 28, lot
	// U4). A ticker typed here is not written back: it carries no market.
	const context = useMarketContext();
	const [ticker, setTicker] = useState(context?.ticker ?? "AAPL");
	const [calibrated, setCalibrated] =
		useState<SimulationCalibrateResponse | null>(null);

	// Heston params (also the SLV's, read-only there: its leverage is
	// calibrated with them)
	const heston = useHestonParams();

	const calibrate = useCalibrateSimulation();
	const simulate = useSimulationDistribution();
	const surfaceModel =
		model === "local_vol" || model === "heston" || model === "slv";

	const { revealCount, setRevealCount, playAnimation } = usePathReveal();
	// The date the histogram and the stats are read at: the last one by default.
	const [cursor, setCursor] = useState(0);

	const data = simulate.data;
	useEffect(() => {
		if (!data) return;
		setCursor(data.time_grid.length - 1);
		playAnimation(data.time_grid.length - 1);
	}, [data, playAnimation]);

	function runCalibration() {
		setCalibrated(null);
		calibrate.mutate(
			{
				ticker,
				model,
				ttm: Number(ttm),
				rate: Number(rate) / 100,
				beta: Number(beta),
			},
			{
				onSuccess: (resp) => {
					setCalibrated(resp);
					setTtm(String(resp.ttm));
					if (resp.heston) heston.fromFit(resp.heston);
					if (resp.model !== "sabr") {
						setSpot(String(resp.spot));
						setDividend(String(resp.dividend * 100));
					}
					if (resp.model === "black_scholes") {
						setSpot(String(resp.spot));
						setDividend(String(resp.dividend * 100));
						if (resp.vol != null) setVol(String(resp.vol * 100));
					} else {
						setForward(String(resp.forward));
						if (resp.alpha != null) setAlpha(String(resp.alpha));
						if (resp.beta != null) setBeta(String(resp.beta));
						if (resp.rho != null) setRho(String(resp.rho));
						if (resp.nu != null) setNu(String(resp.nu));
					}
				},
			},
		);
	}

	/** The request for the model and parameters on screen: a ticker when the
	 * model reads its surface or was calibrated on one (the stored surface is
	 * then returned beside the model), the typed parameters otherwise. */
	function request(): DistributionRequest {
		const onTicker =
			model === "local_vol" ||
			model === "slv" ||
			(mode === "calibrate" && calibrated?.model === model);
		const base = {
			model,
			rate: Number(rate) / 100,
			ttm: Number(ttm),
			n_steps: Number(nSteps),
			n_paths: Number(nPaths),
			seed: Number(seed),
			n_display: 30,
			barrier: BARRIER,
			barrier_strike: BARRIER_STRIKE,
			...(onTicker
				? { ticker }
				: { spot: Number(model === "sabr" ? forward : spot) }),
			dividend: Number(dividend) / 100,
		};
		if (model === "black_scholes") return { ...base, vol: Number(vol) / 100 };
		if (model === "sabr")
			return {
				...base,
				sabr: {
					alpha: Number(alpha),
					beta: Number(beta),
					rho: Number(rho),
					nu: Number(nu),
				},
			};
		if (model === "heston") return { ...base, heston: heston.toParams() };
		return base;
	}

	function runSimulation() {
		setRevealCount(0);
		simulate.mutate(request());
	}

	return {
		model,
		setModel,
		mode,
		setMode,
		ttm,
		setTtm,
		nSteps,
		setNSteps,
		nPaths,
		setNPaths,
		seed,
		setSeed,
		spot,
		setSpot,
		rate,
		setRate,
		dividend,
		setDividend,
		vol,
		setVol,
		forward,
		setForward,
		alpha,
		setAlpha,
		beta,
		setBeta,
		rho,
		setRho,
		nu,
		setNu,
		ticker,
		setTicker,
		heston,
		surfaceModel,
		calibrated,
		setCalibrated,
		calibrate,
		simulate,
		runCalibration,
		runSimulation,
		request,
		playAnimation,
		revealCount,
		cursor,
		setCursor,
		data,
	};
}

export type UseSimulation = ReturnType<typeof useSimulation>;
