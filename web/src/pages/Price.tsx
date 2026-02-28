import BondFields from "./pricing/BondFields";
import ExoticFields from "./pricing/ExoticFields";
import FutureFields from "./pricing/FutureFields";
import ModelSelector from "./pricing/ModelSelector";
import OptionFields from "./pricing/OptionFields";
import ResultsPanel from "./pricing/ResultsPanel";
import type { CategoryType } from "./pricing/types";
import { usePricing } from "./pricing/usePricing";

const CATEGORIES: { key: CategoryType; label: string }[] = [
	{ key: "vanilla", label: "Vanilla" },
	{ key: "exotics", label: "Exotics" },
	{ key: "fixed-income", label: "Fixed Income" },
	{ key: "structured", label: "Structured" },
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
								className={`pill${p.category === c.key ? " active" : ""}${c.key === "structured" ? " pill-disabled" : ""}`}
								type="button"
								disabled={c.key === "structured"}
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
										<option value="cliquet">Cliquet</option>
									</select>
								</label>
								<ModelSelector {...p} />
								<ExoticFields {...p} />
							</>
						)}

						{/* ── Fixed Income ─────────────────────── */}
						{p.category === "fixed-income" && <BondFields {...p} />}

						{/* ── Structured (coming soon) ────────── */}
						{p.category === "structured" && (
							<p className="price-muted span-2">Structured products coming soon.</p>
						)}
					</div>

					<div className="price-actions">
						<button className="button" type="submit" disabled={p.loading || p.category === "structured"}>
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
