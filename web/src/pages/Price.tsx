import { useMemo, useState } from "react";

type InstrumentType = "option" | "future" | "bond";
type EngineType = "analytic" | "mc";
type ProductType = "vanilla" | "asian";
type AverageType = "arithmetic" | "geometric";
type CouponFrequency = "annual" | "semiannual" | "quarterly";

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

export default function Price() {
	const [product, setProduct] = useState<ProductType>("vanilla");
	const [instrument, setInstrument] = useState<InstrumentType>("option");
	const [engine, setEngine] = useState<EngineType>("analytic");
	const [averageType, setAverageType] = useState<AverageType>("arithmetic");
	const [isCall, setIsCall] = useState(true);
	const [spot, setSpot] = useState(100);
	const [strike, setStrike] = useState(100);
	const [maturity, setMaturity] = useState(1);
	const [rate, setRate] = useState(0.02);
	const [dividend, setDividend] = useState(0.0);
	const [vol, setVol] = useState(0.2);
	const [forwardPrice, setForwardPrice] = useState(100);
	const [faceValue, setFaceValue] = useState(1000);
	const [couponRate, setCouponRate] = useState(0.05);
	const [yieldRate, setYieldRate] = useState(0.04);
	const [couponFrequency, setCouponFrequency] = useState<CouponFrequency>("annual");
	const [nPaths, setNPaths] = useState(200000);
	const [seed, setSeed] = useState(1);
	const [mcEpsilon, setMcEpsilon] = useState(0.0);
	const [loading, setLoading] = useState(false);
	const [error, setError] = useState<string | null>(null);
	const [result, setResult] = useState<PricingResponse | null>(null);

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
	const endpoint = product === "asian" ? "/price/asian" : "/price/vanilla";

	const handleSubmit = async (event: React.FormEvent<HTMLFormElement>) => {
		event.preventDefault();
		if (instrument !== "option") {
			setError("Only option pricing is wired to the API right now.");
			return;
		}
		setLoading(true);
		setError(null);
		setResult(null);
		try {
			const payload: Record<string, unknown> = {
				spot,
				strike,
				maturity,
				rate,
				dividend,
				vol,
				is_call: isCall,
				engine,
				n_paths: nPaths,
				seed,
				mc_epsilon: mcEpsilon
			};
			if (product === "asian") {
				payload.average_type = averageType;
			}
			const response = await fetch(`${apiBase}${endpoint}`,
				{
					method: "POST",
					headers: { "Content-Type": "application/json" },
					body: JSON.stringify(payload)
				}
			);
			if (!response.ok) {
				const text = await response.text();
				throw new Error(text || "Pricing request failed.");
			}
			const data = (await response.json()) as PricingResponse;
			setResult(data);
		} catch (err) {
			const message = err instanceof Error ? err.message : "Unexpected error.";
			setError(message);
		} finally {
			setLoading(false);
		}
	};

	return (
		<>
			<section className="hero">
				<div>
					<h1>Multi-asset pricing desk.</h1>
					<p>
						Pick the instrument, configure its terms, and price through the API. Options are live today, with more
						instruments coming online next.
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
									<option value="vanilla">Vanilla</option>
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
						{instrument === "option" && (
							<label className="field">
								Pricing engine
								<select value={engine} onChange={(event) => setEngine(event.target.value as EngineType)}>
									<option value="analytic">Analytic</option>
									<option value="mc">Monte Carlo</option>
								</select>
							</label>
						)}
						{instrument === "option" && product === "asian" && (
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
								<label className="field">
									Paths (MC)
									<input
										type="number"
										step="10000"
										disabled={engine !== "mc"}
										value={nPaths}
										onChange={(event) => setNPaths(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									Seed (MC)
									<input
										type="number"
										disabled={engine !== "mc"}
										value={seed}
										onChange={(event) => setSeed(Number(event.target.value))}
									/>
								</label>
								<label className="field">
									MC epsilon
									<input
										type="number"
										step="0.0001"
										disabled={engine !== "mc"}
										value={mcEpsilon}
										onChange={(event) => setMcEpsilon(Number(event.target.value))}
									/>
								</label>
							</>
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
									Forward price
									<input
										type="number"
										step="1"
										value={forwardPrice}
										onChange={(event) => setForwardPrice(Number(event.target.value))}
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
							</>
						)}
						{instrument === "bond" && (
							<>
								<label className="field">
									Face value
									<input
										type="number"
										step="1"
										value={faceValue}
										onChange={(event) => setFaceValue(Number(event.target.value))}
									/>
								</label>
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
									Yield
									<input
										type="number"
										step="0.01"
										value={yieldRate}
										onChange={(event) => setYieldRate(Number(event.target.value))}
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
					</div>
					<div className="price-actions">
						<button className="button" type="submit" disabled={loading || instrument !== "option"}>
							{loading ? "Pricing..." : "Run pricing"}
						</button>
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
