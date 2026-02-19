import { useMemo, useRef, useState } from "react";

type InstrumentType = "option" | "future" | "bond";
type EngineType = "analytic" | "mc" | "binomial" | "trinomial" | "pde";
type ProductType = "vanilla" | "asian" | "american";
type AverageType = "arithmetic" | "geometric";
type CouponFrequency = "annual" | "semiannual" | "quarterly";
type BondType = "zero-coupon" | "fixed-rate";
type CurvePoint = { time: number; df: number };

type PricingResponse = {
	npv: number;
	greeks: {
		delta?: number | null;
		gamma?: number | null;
		vega?: number | null;
		theta?: number | null;
		rho?: number | null;
		delta_std_error?: number | null;
		gamma_std_error?: number | null;
		vega_std_error?: number | null;
		theta_std_error?: number | null;
		rho_std_error?: number | null;
	};
	diagnostics?: string | null;
	mc_std_error?: number | null;
};

const REQUEST_TIMEOUT_MS = 20_000;

export default function Price() {
	const [product, setProduct] = useState<ProductType>("vanilla");
	const [instrument, setInstrument] = useState<InstrumentType>("option");
	const [engine, setEngine] = useState<EngineType>("analytic");
	const [averageType, setAverageType] = useState<AverageType>("arithmetic");
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
	const [curvePoints, setCurvePoints] = useState<CurvePoint[]>([
		{ time: 0.5, df: 0.985 },
		{ time: 1, df: 0.97 },
		{ time: 2, df: 0.94 },
		{ time: 3, df: 0.91 },
		{ time: 5, df: 0.86 }
	]);
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
	const fairForward = useMemo(() => spot * Math.exp((rate - dividend) * maturity), [spot, rate, dividend, maturity]);

	const formatNumber = (value?: number | null) =>
		value === null || value === undefined ? "-" : value.toFixed(3);
	const formatWithError = (value?: number | null, errorValue?: number | null) => {
		const formatted = formatNumber(value);
		if (formatted === "-") {
			return "-";
		}
		const errorFormatted = formatNumber(errorValue);
		if (errorFormatted === "-") {
			return formatted;
		}
		return `${formatted} +/- ${errorFormatted}`;
	};

	const apiBase = useMemo(() => import.meta.env.VITE_API_BASE ?? "/api", []);
	const bondFrequencyMap: Record<CouponFrequency, number> = {
		annual: 1,
		semiannual: 2,
		quarterly: 4
	};
	const sampleCurve = useMemo<CurvePoint[]>(
		() => [
			{ time: 0.25, df: 0.992 },
			{ time: 0.5, df: 0.985 },
			{ time: 1, df: 0.97 },
			{ time: 2, df: 0.945 },
			{ time: 3, df: 0.92 },
			{ time: 5, df: 0.88 }
		],
		[]
	);

	const buildCurvePayload = () => {
		if (!useCurve) {
			return { discount_times: [], discount_factors: [] };
		}
		const cleaned = curvePoints
			.map((point) => ({ time: Number(point.time), df: Number(point.df) }))
			.filter((point) => Number.isFinite(point.time) && Number.isFinite(point.df) && point.time > 0 && point.df > 0)
			.sort((a, b) => a.time - b.time);
		return {
			discount_times: cleaned.map((point) => point.time),
			discount_factors: cleaned.map((point) => point.df)
		};
	};

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
			if (instrument === "future") {
				endpoint = "/price/future";
				payload = {
					spot,
					strike,
					maturity,
					rate,
					dividend,
					notional
				};
			} else if (instrument === "bond") {
				endpoint = bondType === "zero-coupon" ? "/price/bond/zero-coupon" : "/price/bond/fixed-rate";
				payload = {
					maturity,
					rate: yieldRate,
					notional: faceValue,
					...curvePayload
				};
				if (bondType === "fixed-rate") {
					payload.coupon_rate = couponRate;
					payload.coupon_frequency = bondFrequencyMap[couponFrequency];
				}
			} else {
				endpoint = "/price/option/vanilla";
				payload = {
					spot,
					strike,
					maturity,
					rate,
					dividend,
					vol,
					is_call: isCall,
					is_american: product === "american",
					engine,
					n_paths: nPaths,
					seed,
					mc_epsilon: mcEpsilon,
					tree_steps: treeSteps,
					pde_space_steps: pdeSpaceSteps,
					pde_time_steps: pdeTimeSteps
				};
				if (product === "asian") {
					endpoint = "/price/option/asian";
					delete payload.is_american;
					delete payload.tree_steps;
					payload.average_type = averageType;
				}
			}
			const response = await fetch(`${apiBase}${endpoint}`,
				{
					method: "POST",
					headers: { "Content-Type": "application/json" },
					body: JSON.stringify(payload),
					signal: controller.signal
				}
			);
			if (!response.ok) {
				const text = await response.text();
				throw new Error(text || "Pricing request failed.");
			}
			const data = (await response.json()) as PricingResponse;
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
			if (abortControllerRef.current === controller) {
				abortControllerRef.current = null;
			}
			setLoading(false);
		}
	};

	return (
		<>
			<section className="hero">
				<div>
					<h1>Multi-asset pricing desk.</h1>
					<p>
						Pick the instrument, configure its terms, and price through the API. Options, futures, and bonds are
						available with curve-based discounting.
					</p>
				</div>
			</section>

			<section className="price-grid">
				<form className="card" onSubmit={handleSubmit}>
					<h2>Configure pricing</h2>
					<div className="grid-2">
						<label className="field">
							Instrument
							<select value={instrument} onChange={(event) => setInstrument(event.target.value as InstrumentType)}>
								<option value="option">Option</option>
								<option value="future">Future</option>
								<option value="bond">Bond</option>
							</select>
						</label>
						{instrument === "option" && (
							<label className="field">
								Product
								<select value={product} onChange={(event) => setProduct(event.target.value as ProductType)}>
								<option value="vanilla">Vanilla (European)</option>
								<option value="american">Vanilla (American)</option>
									<option value="asian">Asian</option>
								</select>
							</label>
						)}
						{instrument === "option" && (
							<label className="field">
								Option type
								<select value={isCall ? "call" : "put"} onChange={(event) => setIsCall(event.target.value === "call")}>
									<option value="call">Call</option>
									<option value="put">Put</option>
								</select>
							</label>
						)}
					{instrument === "option" && product === "vanilla" && (
						<label className="field">
							Pricing engine
							<select value={engine} onChange={(event) => setEngine(event.target.value as EngineType)}>
								<option value="analytic">Analytic (Black-Scholes)</option>
								<option value="mc">Monte Carlo</option>
								<option value="binomial">Binomial Tree</option>
								<option value="trinomial">Trinomial Tree</option>
								<option value="pde">PDE Crank-Nicolson</option>
							</select>
						</label>
					)}
					{instrument === "option" && product === "american" && (
						<label className="field">
							Pricing engine
							<select value={engine} onChange={(event) => setEngine(event.target.value as EngineType)}>
								<option value="binomial">Binomial Tree</option>
								<option value="trinomial">Trinomial Tree</option>
							</select>
						</label>
					)}
						{instrument === "option" && product === "asian" && (
							<>
								<label className="field">
									Pricing engine
									<select value={engine} onChange={(event) => setEngine(event.target.value as EngineType)}>
										<option value="analytic">Analytic</option>
										<option value="mc">Monte Carlo</option>
									</select>
								</label>
								<label className="field">
									Average type
									<select
										value={averageType}
										onChange={(event) => setAverageType(event.target.value as AverageType)}
									>
										<option value="arithmetic">Arithmetic</option>
										<option value="geometric">Geometric</option>
									</select>
								</label>
							</>
						)}
						{instrument === "option" && (
							<>
								<label className="field">
									Spot
									<input
										type="number"
										step="1"
										value={spot}
										onChange={(event) => setSpot(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Strike
									<input
										type="number"
										step="1"
										value={strike}
										onChange={(event) => setStrike(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Maturity (years)
									<input
										type="number"
										step="0.01"
										value={maturity}
										onChange={(event) => setMaturity(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Rate
									<input
										type="number"
										step="0.01"
										value={rate}
										onChange={(event) => setRate(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Dividend
									<input
										type="number"
										step="0.01"
										value={dividend}
										onChange={(event) => setDividend(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Volatility
									<input
										type="number"
										step="0.01"
										value={vol}
										onChange={(event) => setVol(Number(event.target.value))}
									/>
								</label>
								{engine === "mc" && (
									<>
										<label className="field">
											Paths (MC)
											<input
												type="number"
												step="10000"
												value={nPaths}
												onChange={(event) => setNPaths(Number(event.target.value))}
											/>
										</label>
										<label className="field">
											Seed (MC)
											<input
												type="number"
												value={seed}
												onChange={(event) => setSeed(Number(event.target.value))}
											/>
										</label>
										<label className="field">
											MC epsilon
											<input
												type="number"
												step="0.0001"
												value={mcEpsilon}
												onChange={(event) => setMcEpsilon(Number(event.target.value))}
											/>
										</label>
									</>
								)}
							</>
						)}
						{instrument === "option" && product === "vanilla" && (engine === "binomial" || engine === "trinomial") && (
							<label className="field">
								Tree steps
								<input
									type="number"
									step="10"
									value={treeSteps}
									onChange={(event) => setTreeSteps(Number(event.target.value))}
								/>
							</label>
						)}
						{instrument === "option" && product === "vanilla" && engine === "pde" && (
							<>
								<label className="field">
									PDE space steps
									<input
										type="number"
										step="10"
										value={pdeSpaceSteps}
										onChange={(event) => setPdeSpaceSteps(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									PDE time steps
									<input
										type="number"
										step="10"
										value={pdeTimeSteps}
										onChange={(event) => setPdeTimeSteps(Number(event.target.value))}
									/>
								</label>
							</>
						)}
						{instrument === "option" && product === "american" && (
							<label className="field">
								Tree steps
								<input
									type="number"
									step="10"
									value={treeSteps}
									onChange={(event) => setTreeSteps(Number(event.target.value))}
								/>
							</label>
						)}
						{instrument === "future" && (
							<>
								<label className="field">
									Spot
									<input
										type="number"
										step="1"
										value={spot}
										onChange={(event) => setSpot(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Strike
									<input
										type="number"
										step="1"
										value={strike}
										onChange={(event) => setStrike(Number(event.target.value))}
									/>
								</label>
								<div style={{ display: "flex", alignItems: "center", gap: 12, gridColumn: "1 / -1" }}>
									<button
										className="button secondary"
										type="button"
										disabled={!Number.isFinite(fairForward)}
										onClick={() => setStrike(Number(fairForward.toFixed(4)))}
									>
										Set strike to fair forward
									</button>
									<span className="price-muted">
										Fair forward: {Number.isFinite(fairForward) ? fairForward.toFixed(4) : "-"}
									</span>
								</div>
								<label className="field">
									Maturity (years)
									<input
										type="number"
										step="0.01"
										value={maturity}
										onChange={(event) => setMaturity(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Rate
									<input
										type="number"
										step="0.01"
										value={rate}
										onChange={(event) => setRate(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Dividend
									<input
										type="number"
										step="0.01"
										value={dividend}
										onChange={(event) => setDividend(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Notional
									<input
										type="number"
										step="1"
										value={notional}
										onChange={(event) => setNotional(Number(event.target.value))}
									/>
								</label>
							</>
						)}
						{instrument === "bond" && (
							<>
								<div className="bond-toggle span-2">
									<button
										className={`pill ${bondType === "fixed-rate" ? "active" : ""}`}
										type="button"
										onClick={() => setBondType("fixed-rate")}
									>
										Fixed-rate bond
									</button>
									<button
										className={`pill ${bondType === "zero-coupon" ? "active" : ""}`}
										type="button"
										onClick={() => setBondType("zero-coupon")}
									>
										Zero-coupon
									</button>
								</div>
								<label className="field">
									Notional
									<input
										type="number"
										step="1000"
										value={faceValue}
										onChange={(event) => setFaceValue(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Maturity (years)
									<input
										type="number"
										step="1"
										value={maturity}
										onChange={(event) => setMaturity(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Flat rate (fallback)
									<input
										type="number"
										step="0.01"
										value={yieldRate}
										onChange={(event) => setYieldRate(Number(event.target.value))}
									/>
								</label>
								{bondType === "fixed-rate" && (
									<>
										<label className="field">
											Coupon rate
											<input
												type="number"
												step="0.01"
												value={couponRate}
												onChange={(event) => setCouponRate(Number(event.target.value))}
											/>
										</label>
										<label className="field">
											Coupon frequency
											<select
												value={couponFrequency}
												onChange={(event) => setCouponFrequency(event.target.value as CouponFrequency)}
											>
												<option value="annual">Annual</option>
												<option value="semiannual">Semi-annual</option>
												<option value="quarterly">Quarterly</option>
											</select>
										</label>
									</>
								)}
								<div className="field span-2">
									<label className="curve-toggle">
										<input
											type="checkbox"
											checked={useCurve}
											onChange={(event) => setUseCurve(event.target.checked)}
										/>
										Use discount curve
									</label>
								</div>
								{useCurve && (
									<div className="curve-editor span-2">
										<div className="curve-grid">
											<div className="curve-row curve-head">
												<span>Time (y)</span>
												<span>Discount factor</span>
												<span />
											</div>
											{curvePoints.map((point, idx) => (
												<div className="curve-row" key={`curve-${idx}`}>
													<input
														type="number"
														step="0.25"
														value={point.time}
														onChange={(event) => {
															const next = [...curvePoints];
															next[idx] = { ...next[idx], time: Number(event.target.value) };
															setCurvePoints(next);
														}}
													/>
													<input
														type="number"
														step="0.01"
														value={point.df}
														onChange={(event) => {
															const next = [...curvePoints];
															next[idx] = { ...next[idx], df: Number(event.target.value) };
															setCurvePoints(next);
														}}
													/>
													<button
														className="button ghost"
														type="button"
														onClick={() =>
															setCurvePoints((prev) => prev.filter((_, index) => index !== idx))
														}
													>
														Remove
													</button>
												</div>
											))}
										</div>
										<div className="curve-actions">
											<button
												className="button secondary"
												type="button"
												onClick={() => setCurvePoints((prev) => [...prev, { time: 6, df: 0.85 }])}
											>
												Add point
											</button>
											<button
												className="button secondary"
												type="button"
												onClick={() => setCurvePoints(sampleCurve)}
											>
												Load sample
											</button>
											<button
												className="button ghost"
												type="button"
												onClick={() => setCurvePoints([])}
											>
												Clear
											</button>
										</div>
										<p className="curve-hint">Times in years. Discount factors must be positive.</p>
									</div>
								)}
							</>
						)}
					</div>
					<div className="price-actions">
						<button className="button" type="submit" disabled={loading}>
							{loading ? "Pricing..." : "Run pricing"}
						</button>
						{loading && (
							<button className="button secondary" type="button" onClick={stopPricing}>
								Stop
							</button>
						)}
						{error && <span className="price-error">{error}</span>}
					</div>
				</form>

				<section className="card">
					<h2>Results</h2>
					{!result && !loading && (
						<p className="price-muted">Run a pricing request to see NPV and Greeks.</p>
					)}
					{result && (
						<div className="result-stack">
							<div className="result-kv">
								<span className="result-label">NPV</span>
								<span className="result-value">{formatWithError(result.npv, result.mc_std_error)}</span>
							</div>
							<div className="result-grid">
								<div>
									<span className="result-label">Delta</span>
									<span className="result-value">
										{formatWithError(result.greeks.delta, result.greeks.delta_std_error)}
									</span>
								</div>
								<div>
									<span className="result-label">Gamma</span>
									<span className="result-value">
										{formatWithError(result.greeks.gamma, result.greeks.gamma_std_error)}
									</span>
								</div>
								<div>
									<span className="result-label">Vega</span>
									<span className="result-value">
										{formatWithError(result.greeks.vega, result.greeks.vega_std_error)}
									</span>
								</div>
								<div>
									<span className="result-label">Theta</span>
									<span className="result-value">
										{formatWithError(result.greeks.theta, result.greeks.theta_std_error)}
									</span>
								</div>
								<div>
									<span className="result-label">Rho</span>
									<span className="result-value">
										{formatWithError(result.greeks.rho, result.greeks.rho_std_error)}
									</span>
								</div>
							</div>
							{/* {result.diagnostics && (
								<div className="result-note">
									<span className="result-label">Diagnostics</span>
									<span className="result-value">{result.diagnostics}</span>
								</div>
							)} */}
						</div>
					)}
				</section>
			</section>
		</>
	);
}
