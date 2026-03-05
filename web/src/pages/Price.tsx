import BondFields from "./pricing/BondFields";
import CommodityFields from "./pricing/CommodityFields";
import ExoticFields from "./pricing/ExoticFields";
import FutureFields from "./pricing/FutureFields";
import FXFields from "./pricing/FXFields";
import ModelSelector from "./pricing/ModelSelector";
import OptionFields from "./pricing/OptionFields";
import {
	CATEGORIES,
	COMMODITY_PRODUCTS,
	EXOTIC_PRODUCTS,
	FX_PRODUCTS,
	isEnabled,
	type ProductEntry,
	STRUCTURED_PRODUCTS,
	VANILLA_INSTRUMENTS,
	VOL_PRODUCTS,
} from "./pricing/productRegistry";
import ResultsPanel from "./pricing/ResultsPanel";
import StructuredFields from "./pricing/StructuredFields";
import VolatilityFields from "./pricing/VolatilityFields";
import { usePricing } from "./pricing/usePricing";

/** Render <option> elements from a registry list, greying out disabled ones. */
function registryOptions<K extends string>(list: ProductEntry<K>[]) {
	return list.map((p) => (
		<option key={p.key} value={p.key} disabled={!p.enabled}>
			{p.label}{!p.enabled ? " (coming soon)" : ""}
		</option>
	));
}

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
								className={`pill${p.category === c.key ? " active" : ""}${!c.enabled ? " pill-disabled" : ""}`}
								type="button"
								onClick={() => c.enabled && p.setCategory(c.key)}
								title={!c.enabled ? "Not yet vetted" : undefined}
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
								<select
									value={p.instrument}
									onChange={(e) => {
										const v = e.target.value as typeof p.instrument;
										if (isEnabled(VANILLA_INSTRUMENTS, v)) p.setInstrument(v);
									}}
								>
									{registryOptions(VANILLA_INSTRUMENTS)}
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
								<select
									value={p.exoticProduct}
									onChange={(e) => {
										const v = e.target.value as typeof p.exoticProduct;
										if (isEnabled(EXOTIC_PRODUCTS, v)) p.setExoticProduct(v);
									}}
								>
									{registryOptions(EXOTIC_PRODUCTS)}
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
								<select
									value={p.structuredProduct}
									onChange={(e) => {
										const v = e.target.value as typeof p.structuredProduct;
										if (isEnabled(STRUCTURED_PRODUCTS, v)) p.setStructuredProduct(v);
									}}
								>
									{registryOptions(STRUCTURED_PRODUCTS)}
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
								<select
									value={p.volProduct}
									onChange={(e) => {
										const v = e.target.value as typeof p.volProduct;
										if (isEnabled(VOL_PRODUCTS, v)) p.setVolProduct(v);
									}}
								>
									{registryOptions(VOL_PRODUCTS)}
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
								<select
									value={p.fxProduct}
									onChange={(e) => {
										const v = e.target.value as typeof p.fxProduct;
										if (isEnabled(FX_PRODUCTS, v)) p.setFxProduct(v);
									}}
								>
									{registryOptions(FX_PRODUCTS)}
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
								<select
									value={p.commodityProduct}
									onChange={(e) => {
										const v = e.target.value as typeof p.commodityProduct;
										if (isEnabled(COMMODITY_PRODUCTS, v)) p.setCommodityProduct(v);
									}}
								>
									{registryOptions(COMMODITY_PRODUCTS)}
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
