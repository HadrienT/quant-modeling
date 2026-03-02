import BondFields from "./pricing/BondFields";
import CommodityFields from "./pricing/CommodityFields";
import ExoticFields from "./pricing/ExoticFields";
import FutureFields from "./pricing/FutureFields";
import FXFields from "./pricing/FXFields";
import ModelSelector from "./pricing/ModelSelector";
import OptionFields from "./pricing/OptionFields";
import ResultsPanel from "./pricing/ResultsPanel";
import StructuredFields from "./pricing/StructuredFields";
import type { CategoryType } from "./pricing/types";
import VolatilityFields from "./pricing/VolatilityFields";
import { usePricing } from "./pricing/usePricing";

const CATEGORIES: { key: CategoryType; label: string }[] = [
	{ key: "vanilla", label: "Vanilla" },
	{ key: "exotics", label: "Exotics" },
	{ key: "fixed-income", label: "Fixed Income" },
	{ key: "structured", label: "Structured" },
	{ key: "volatility", label: "Volatility" },
	{ key: "fx", label: "FX" },
	{ key: "commodity", label: "Commodity" },
];

export default function Price() {
	const p = usePricing();

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
				<form className="card" onSubmit={p.handleSubmit}>
					{/* ── Category pill bar ──────────────────── */}
					<div className="category-bar">
						{CATEGORIES.map((c) => (
							<button
								key={c.key}
								className={`pill${p.category === c.key ? " active" : ""}`}
								type="button"
								onClick={() => p.setCategory(c.key)}
							>
								{c.label}
							</button>
						))}
					</div>

					<h2>Configure pricing</h2>
					<div className="grid-2">
						{/* ── Vanilla: instrument picker + fields ── */}
						{p.category === "vanilla" && (
							<>
								<label className="field">
									Instrument
									<select value={p.instrument} onChange={(e) => p.setInstrument(e.target.value as typeof p.instrument)}>
										<option value="option">Option</option>
										<option value="future">Future</option>
									</select>
								</label>
								{p.instrument === "option" && (
									<>
										<label className="field">
											Option type
											<select value={p.isCall ? "call" : "put"} onChange={(e) => p.setIsCall(e.target.value === "call")}>
												<option value="call">Call</option>
												<option value="put">Put</option>
											</select>
										</label>
										<ModelSelector {...p} />
										<OptionFields {...p} />
									</>
								)}
								{p.instrument === "future" && <FutureFields {...p} />}
							</>
						)}

						{/* ── Exotics ──────────────────────────── */}
						{p.category === "exotics" && (
							<>
								<label className="field">
									Exotic product
									<select value={p.exoticProduct} onChange={(e) => p.setExoticProduct(e.target.value as typeof p.exoticProduct)}>
										<option value="barrier">Barrier</option>
										<option value="digital">Digital</option>
										<option value="lookback">Lookback</option>
										<option value="basket">Basket</option>
										<option value="rainbow">Rainbow</option>
									</select>
								</label>
								<ModelSelector {...p} />
								<ExoticFields {...p} />
							</>
						)}

						{/* ── Fixed Income ─────────────────────── */}
						{p.category === "fixed-income" && <BondFields {...p} />}

						{/* ── Structured ──────────────────────── */}
						{p.category === "structured" && (
							<>
								<label className="field">
									Structured product
									<select value={p.structuredProduct} onChange={(e) => p.setStructuredProduct(e.target.value as typeof p.structuredProduct)}>
										<option value="autocall">Autocall</option>
										<option value="mountain">Mountain / Himalaya</option>
									</select>
								</label>
								<StructuredFields {...p} />
							</>
						)}

						{/* ── Volatility ──────────────────────── */}
						{p.category === "volatility" && (
							<>
								<label className="field">
									Volatility product
									<select value={p.volProduct} onChange={(e) => p.setVolProduct(e.target.value as typeof p.volProduct)}>
										<option value="variance-swap">Variance Swap</option>
										<option value="volatility-swap">Volatility Swap</option>
										<option value="dispersion-swap">Dispersion Swap</option>
									</select>
								</label>
								<VolatilityFields {...p} />
							</>
						)}

						{/* ── FX ────────────────────────────────── */}
						{p.category === "fx" && (
							<>
								<label className="field">
									FX product
									<select value={p.fxProduct} onChange={(e) => p.setFxProduct(e.target.value as typeof p.fxProduct)}>
										<option value="fx-forward">FX Forward</option>
										<option value="fx-option">FX Option (Garman–Kohlhagen)</option>
									</select>
								</label>
								<FXFields {...p} />
							</>
						)}

						{/* ── Commodity ─────────────────────────── */}
						{p.category === "commodity" && (
							<>
								<label className="field">
									Commodity product
									<select value={p.commodityProduct} onChange={(e) => p.setCommodityProduct(e.target.value as typeof p.commodityProduct)}>
										<option value="commodity-forward">Commodity Forward</option>
										<option value="commodity-option">Commodity Option (Black '76)</option>
									</select>
								</label>
								<CommodityFields {...p} />
							</>
						)}
					</div>

					<div className="price-actions">
						<button className="button" type="submit" disabled={p.loading}>
							{p.loading ? "Pricing..." : "Run pricing"}
						</button>
						{p.loading && (
							<button className="button secondary" type="button" onClick={p.stopPricing}>
								Stop
							</button>
						)}
						{p.error && <span className="price-error">{p.error}</span>}
					</div>
				</form>

				<ResultsPanel result={p.result} loading={p.loading} category={p.category} formatWithError={p.formatWithError} />
			</section>
		</>
	);
}
