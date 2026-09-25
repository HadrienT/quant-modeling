import { useEffect, useState } from "react";
import {
	useCalibrateSimulation,
	useSimulateBlackScholesPaths,
	useSimulateModelPaths,
	useSimulateSabrPaths,
} from "@/shared/api";
import type {
	SimulationCalibrateResponse,
	SimulationModel,
} from "@/shared/api/types";
import { useHestonParams } from "./useHestonParams";
import { usePathReveal } from "./usePathReveal";

/**
 * All state and side effects for the simulation page: model/mode selection,
 * every parameter field (shared, BS-specific, SABR-specific), the calibrate-
 * on-ticker flow, the two path-simulation mutations, and the client-side
 * "drawing" animation over an already-fetched batch of paths. Kept out of
 * SimulationPage.tsx to stay under this project's per-feature-file line
 * budget (tests/discipline.test.ts) — same rationale as pricing's
 * useWorkbench.ts.
 */
export function useSimulation() {
	const [model, setModel] = useState<SimulationModel>("black_scholes");
	const [mode, setMode] = useState<"manual" | "calibrate">("manual");

	// Shared
	const [ttm, setTtm] = useState("1.0");
	const [nSteps, setNSteps] = useState("100");
	const [nPaths, setNPaths] = useState("30");
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
	const [ticker, setTicker] = useState("AAPL");
	const [calibrated, setCalibrated] =
		useState<SimulationCalibrateResponse | null>(null);

	// Heston params (also the SLV's, read-only there: its leverage is
	// calibrated with them)
	const heston = useHestonParams();

	const calibrate = useCalibrateSimulation();
	const simulateBS = useSimulateBlackScholesPaths();
	const simulateSABR = useSimulateSabrPaths();
	const simulateModel = useSimulateModelPaths();
	const surfaceModel =
		model === "local_vol" || model === "heston" || model === "slv";
	const simulate =
		model === "black_scholes"
			? simulateBS
			: model === "sabr"
				? simulateSABR
				: simulateModel;

	const { revealCount, setRevealCount, playAnimation } = usePathReveal();

	const data = simulate.data;
	useEffect(() => {
		if (data) playAnimation(data.time_grid.length - 1);
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

	function runSimulation() {
		setRevealCount(0);
		const grid = {
			ttm: Number(ttm),
			n_steps: Number(nSteps),
			n_paths: Number(nPaths),
			seed: Number(seed),
		};
		if (surfaceModel) {
			// Local vol and SLV read the ticker's surface; Heston runs on the
			// parameters shown (typed, or filled in by a calibration).
			simulateModel.mutate(
				model === "heston"
					? {
							model,
							spot: Number(spot),
							dividend: Number(dividend) / 100,
							heston: heston.toParams(),
							rate: Number(rate) / 100,
							...grid,
						}
					: { model, ticker, rate: Number(rate) / 100, ...grid },
			);
		} else if (model === "black_scholes") {
			simulateBS.mutate({
				spot: Number(spot),
				rate: Number(rate) / 100,
				dividend: Number(dividend) / 100,
				vol: Number(vol) / 100,
				ttm: Number(ttm),
				n_steps: Number(nSteps),
				n_paths: Number(nPaths),
				seed: Number(seed),
			});
		} else {
			simulateSABR.mutate({
				forward: Number(forward),
				alpha: Number(alpha),
				beta: Number(beta),
				rho: Number(rho),
				nu: Number(nu),
				ttm: Number(ttm),
				n_steps: Number(nSteps),
				n_paths: Number(nPaths),
				seed: Number(seed),
			});
		}
	}

	const chartPaths = data?.paths.map((path) =>
		path.map((value, i) => ({ t: data.time_grid[i] ?? 0, value })),
	);

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
		playAnimation,
		revealCount,
		data,
		chartPaths,
	};
}

export type UseSimulation = ReturnType<typeof useSimulation>;
