import { useEffect } from "react";
import type { BondCurveSource, CurvePoint } from "./types";
import type { PricingHook } from "./usePricing";

type Props = Pick<
	PricingHook,
	| "bondType" | "setBondType"
	| "faceValue" | "setFaceValue"
	| "maturity" | "setMaturity"
	| "yieldRate" | "setYieldRate"
	| "couponRate" | "setCouponRate"
	| "couponFrequency" | "setCouponFrequency"
	| "useCurve" | "setUseCurve"
	| "bondCurveSource" | "setBondCurveSource"
	| "curvePoints" | "setCurvePoints"
	| "sampleCurve"
	| "loadMarketCurve" | "curveLoading"
>;

export default function BondFields(props: Props) {
	const {
		bondType, setBondType,
		faceValue, setFaceValue,
		maturity, setMaturity,
		yieldRate, setYieldRate,
		couponRate, setCouponRate,
		couponFrequency, setCouponFrequency,
		useCurve, setUseCurve,
		bondCurveSource, setBondCurveSource,
		curvePoints, setCurvePoints,
		sampleCurve,
		loadMarketCurve, curveLoading,
	} = props;

	const isMarketCurve = bondCurveSource !== "manual";

	/* auto-fetch when switching to a market curve source */
	useEffect(() => {
		if (isMarketCurve) {
			void loadMarketCurve(bondCurveSource);
		}
	}, [bondCurveSource, isMarketCurve, loadMarketCurve]);

	const updateCurvePoint = (idx: number, field: keyof CurvePoint, raw: string) => {
		const next = [...curvePoints];
		next[idx] = { ...next[idx], [field]: Number(raw) };
		setCurvePoints(next);
	};

	return (
		<>
			<div className="bond-toggle span-2">
				<button className={`pill ${bondType === "fixed-rate" ? "active" : ""}`} type="button" onClick={() => setBondType("fixed-rate")}>
					Fixed-rate bond
				</button>
				<button className={`pill ${bondType === "zero-coupon" ? "active" : ""}`} type="button" onClick={() => setBondType("zero-coupon")}>
					Zero-coupon
				</button>
			</div>

			<label className="field">
				Notional
				<input type="number" step="1000" value={faceValue} onChange={(e) => setFaceValue(Number(e.target.value))} />
			</label>
			<label className="field">
				Maturity (years)
				<input type="number" step="1" value={maturity} onChange={(e) => setMaturity(Number(e.target.value))} />
			</label>
			<label className="field">
				Flat rate (fallback)
				<input type="number" step="0.01" value={yieldRate} onChange={(e) => setYieldRate(Number(e.target.value))} />
			</label>

			{bondType === "fixed-rate" && (
				<>
					<label className="field">
						Coupon rate
						<input type="number" step="0.01" value={couponRate} onChange={(e) => setCouponRate(Number(e.target.value))} />
					</label>
					<label className="field">
						Coupon frequency
						<select value={couponFrequency} onChange={(e) => setCouponFrequency(e.target.value as typeof couponFrequency)}>
							<option value="annual">Annual</option>
							<option value="semiannual">Semi-annual</option>
							<option value="quarterly">Quarterly</option>
						</select>
					</label>
				</>
			)}

			{/* ── Curve source selector ────────────────── */}
			<label className="field">
				Discount curve
				<select
					value={bondCurveSource}
					onChange={(e) => {
						const src = e.target.value as BondCurveSource;
						setBondCurveSource(src);
						setUseCurve(src !== "manual" || useCurve);
					}}
				>
					<option value="Treasury">Treasury (CMT) — live</option>
					<option value="SOFR">SOFR — live</option>
					<option value="FedFunds">Fed Funds — live</option>
					<option value="manual">Manual entry</option>
				</select>
			</label>

			{bondCurveSource === "manual" && (
				<div className="field span-2">
					<label className="curve-toggle">
						<input type="checkbox" checked={useCurve} onChange={(e) => setUseCurve(e.target.checked)} />
						Use discount curve
					</label>
				</div>
			)}

			{/* ── Market curve: read-only display ──────── */}
			{isMarketCurve && (
				<div className="curve-editor span-2">
					{curveLoading && <p className="price-muted">Loading {bondCurveSource} curve…</p>}
					{!curveLoading && curvePoints.length > 0 && (
						<>
							<div className="curve-grid">
								<div className="curve-row curve-head">
									<span>Tenor</span>
									<span>Discount factor</span>
								</div>
								{curvePoints.map((point, idx) => (
									<div className="curve-row" key={`curve-${idx}`}>
										<span className="curve-cell-ro">{point.time >= 1 ? `${point.time}Y` : `${Math.round(point.time * 12)}M`}</span>
										<span className="curve-cell-ro">{point.df.toFixed(6)}</span>
									</div>
								))}
							</div>
							<div className="curve-actions">
								<button className="button secondary" type="button" disabled={curveLoading} onClick={() => { void loadMarketCurve(bondCurveSource); }}>
									Refresh
								</button>
							</div>
						</>
					)}
					{!curveLoading && curvePoints.length === 0 && (
						<p className="price-muted">No curve data available. Check API connectivity.</p>
					)}
				</div>
			)}

			{/* ── Manual curve editor ──────────────────── */}
			{!isMarketCurve && useCurve && (
				<div className="curve-editor span-2">
					<div className="curve-grid">
						<div className="curve-row curve-head">
							<span>Time (y)</span>
							<span>Discount factor</span>
							<span />
						</div>
						{curvePoints.map((point, idx) => (
							<div className="curve-row" key={`curve-${idx}`}>
								<input type="number" step="0.25" value={point.time} onChange={(e) => updateCurvePoint(idx, "time", e.target.value)} />
								<input type="number" step="0.01" value={point.df} onChange={(e) => updateCurvePoint(idx, "df", e.target.value)} />
								<button className="button ghost" type="button" onClick={() => setCurvePoints((prev) => prev.filter((_, i) => i !== idx))}>
									Remove
								</button>
							</div>
						))}
					</div>
					<div className="curve-actions">
						<button className="button secondary" type="button" onClick={() => setCurvePoints((prev) => [...prev, { time: 6, df: 0.85 }])}>
							Add point
						</button>
						<button className="button secondary" type="button" onClick={() => setCurvePoints(sampleCurve)}>
							Load sample
						</button>
						<button className="button ghost" type="button" onClick={() => setCurvePoints([])}>
							Clear
						</button>
					</div>
					<p className="curve-hint">Times in years. Discount factors must be positive.</p>
				</div>
			)}
		</>
	);
}
