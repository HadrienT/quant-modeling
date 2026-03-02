import { useCallback, useMemo, useState } from "react";
import type { Position } from "../../api/client";
import { CATEGORIES, catalogFor, findProduct, type ParamDef, type ProductCategoryKey, type ProductTypeKey } from "./catalog";

type Props = {
	position: Position;
	onClose: () => void;
	onSave: (updated: Position) => void;
};

function fmt(n: number | null | undefined, dp = 6): string {
	if (n == null) return "—";
	return n.toLocaleString(undefined, { minimumFractionDigits: dp, maximumFractionDigits: dp });
}

function fmtCurrency(n: number | null | undefined): string {
	if (n == null) return "—";
	return n.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
}

function renderParamValue(def: ParamDef | undefined, value: unknown): string {
	if (value == null) return "—";
	if (def?.type === "select") {
		const opt = def.options?.find((o) => String(o.value) === String(value));
		return opt?.label ?? String(value);
	}
	if (def?.type === "boolean") return value ? "Yes" : "No";
	if (Array.isArray(value)) return value.join(", ");
	if (typeof value === "number") return value.toLocaleString(undefined, { maximumFractionDigits: 8 });
	return String(value);
}

export default function PositionDetail({ position, onClose, onSave }: Props) {
	const [editing, setEditing] = useState(false);

	// Editable state — cloned from position
	const [label, setLabel] = useState(position.label);
	const [direction, setDirection] = useState<"long" | "short">(position.direction);
	const [quantity, setQuantity] = useState(position.quantity);
	const [entryPrice, setEntryPrice] = useState(position.entry_price);
	const [category, setCategory] = useState<ProductCategoryKey>(position.category as ProductCategoryKey);
	const [productType, setProductType] = useState<ProductTypeKey>(position.product_type as ProductTypeKey);
	const [params, setParams] = useState<Record<string, unknown>>({ ...position.parameters });

	const productDef = useMemo(() => findProduct(productType), [productType]);
	const products = useMemo(() => catalogFor(category), [category]);
	const r = position.result;

	// Reset edit state from position
	const resetEdit = useCallback(() => {
		setLabel(position.label);
		setDirection(position.direction);
		setQuantity(position.quantity);
		setEntryPrice(position.entry_price);
		setCategory(position.category as ProductCategoryKey);
		setProductType(position.product_type as ProductTypeKey);
		setParams({ ...position.parameters });
	}, [position]);

	const handleCancelEdit = () => {
		resetEdit();
		setEditing(false);
	};

	const handleCategoryChange = (cat: ProductCategoryKey) => {
		setCategory(cat);
		const prods = catalogFor(cat);
		if (prods.length > 0) {
			setProductType(prods[0].type);
			const defaults: Record<string, unknown> = {};
			for (const p of prods[0].params) defaults[p.key] = p.default;
			setParams(defaults);
			setLabel(prods[0].label);
		}
	};

	const handleProductChange = (type: ProductTypeKey) => {
		setProductType(type);
		const def = findProduct(type);
		if (def) {
			const defaults: Record<string, unknown> = {};
			for (const p of def.params) defaults[p.key] = p.default;
			setParams(defaults);
			setLabel(def.label);
		}
	};

	const setParam = useCallback((key: string, value: unknown) => {
		setParams((prev) => ({ ...prev, [key]: value }));
	}, []);

	const handleSave = () => {
		const processed: Record<string, unknown> = { ...params };
		for (const [k, v] of Object.entries(processed)) {
			if (v === "true") processed[k] = true;
			else if (v === "false") processed[k] = false;
		}
		onSave({
			...position,
			label,
			product_type: productType,
			category,
			direction,
			quantity,
			entry_price: entryPrice,
			parameters: processed,
			result: null, // clear cached result — will need re-price
		});
		setEditing(false);
	};

	return (
		<div className="pos-detail-overlay" onClick={onClose}>
			<div className="pos-detail-panel card" onClick={(e) => e.stopPropagation()}>
				{/* Header */}
				<div className="pos-detail-header">
					<div>
						<h3>{position.label}</h3>
						<span className="pos-detail-subtitle">
							{findProduct(position.product_type as ProductTypeKey)?.label ?? position.product_type}
							{r?.engine ? ` · ${r.engine}` : ""}
						</span>
					</div>
					<div className="pos-detail-header-actions">
						{!editing && (
							<button type="button" className="btn-secondary btn-sm" onClick={() => setEditing(true)}>
								Edit
							</button>
						)}
						<button className="btn-icon" type="button" onClick={onClose}>✕</button>
					</div>
				</div>

				{editing ? (
					/* ═══════════════ EDIT MODE ═══════════════ */
					<div className="pos-detail-edit">
						{/* Category pills */}
						<div className="category-bar">
							{CATEGORIES.map((c) => (
								<button
									key={c.key}
									type="button"
									className={`pill${category === c.key ? " active" : ""}`}
									onClick={() => handleCategoryChange(c.key)}
								>
									{c.label}
								</button>
							))}
						</div>

						{/* Core fields */}
						<div className="add-pos-row">
							<label className="field">
								Product
								<select value={productType} onChange={(e) => handleProductChange(e.target.value as ProductTypeKey)}>
									{products.map((p) => (
										<option key={p.type} value={p.type}>{p.label}</option>
									))}
								</select>
							</label>
							<label className="field">
								Direction
								<select value={direction} onChange={(e) => setDirection(e.target.value as "long" | "short")}>
									<option value="long">Long</option>
									<option value="short">Short</option>
								</select>
							</label>
							<label className="field">
								Quantity
								<input type="number" value={quantity} min={0} step={1} onChange={(e) => setQuantity(+e.target.value)} />
							</label>
							<label className="field">
								Entry price
								<input type="number" value={entryPrice} step={0.01} onChange={(e) => setEntryPrice(+e.target.value)} />
							</label>
						</div>

						<label className="field" style={{ marginBottom: 8 }}>
							Label
							<input type="text" value={label} onChange={(e) => setLabel(e.target.value)} placeholder={productDef?.label || ""} />
						</label>

						{/* Dynamic parameters */}
						<div className="add-pos-params">
							{productDef?.params.map((p) => (
								<label className="field" key={p.key}>
									{p.label}
									{p.type === "select" ? (
										<select
											value={String(params[p.key] ?? p.default ?? "")}
											onChange={(e) => setParam(p.key, e.target.value)}
										>
											{p.options?.map((o) => (
												<option key={o.value} value={o.value}>{o.label}</option>
											))}
										</select>
									) : p.type === "boolean" ? (
										<select
											value={String(params[p.key] ?? p.default ?? false)}
											onChange={(e) => setParam(p.key, e.target.value === "true")}
										>
											<option value="true">Yes</option>
											<option value="false">No</option>
										</select>
									) : p.type === "number[]" ? (
										<input
											type="text"
											value={Array.isArray(params[p.key]) ? (params[p.key] as number[]).join(", ") : String(params[p.key] ?? "")}
											onChange={(e) => {
												const nums = e.target.value.split(",").map((s) => Number(s.trim())).filter((n) => !isNaN(n));
												setParam(p.key, nums);
											}}
											placeholder="e.g. 100, 100"
										/>
									) : (
										<input
											type="number"
											value={params[p.key] != null ? Number(params[p.key]) : (p.default as number) ?? 0}
											min={p.min}
											step={p.step ?? 0.01}
											onChange={(e) => setParam(p.key, +e.target.value)}
										/>
									)}
								</label>
							))}
						</div>

						<div className="add-pos-actions">
							<button type="button" className="btn-secondary" onClick={handleCancelEdit}>Cancel</button>
							<button type="button" className="btn-primary" onClick={handleSave}>Save Changes</button>
						</div>
					</div>
				) : (
					/* ═══════════════ VIEW MODE ═══════════════ */
					<div className="pos-detail-view">
						{/* Position info */}
						<div className="pos-detail-section">
							<h4>Position</h4>
							<div className="pos-detail-grid">
								<div className="pos-detail-item">
									<span className="pos-detail-key">Direction</span>
									<span className={`dir-badge ${direction}`}>{position.direction}</span>
								</div>
								<div className="pos-detail-item">
									<span className="pos-detail-key">Quantity</span>
									<span>{position.quantity}</span>
								</div>
								<div className="pos-detail-item">
									<span className="pos-detail-key">Entry Price</span>
									<span>{fmtCurrency(position.entry_price)}</span>
								</div>
								<div className="pos-detail-item">
									<span className="pos-detail-key">Category</span>
									<span className="pill active" style={{ fontSize: "0.7rem", padding: "2px 8px" }}>{position.category}</span>
								</div>
							</div>
						</div>

						{/* Parameters */}
						<div className="pos-detail-section">
							<h4>Parameters</h4>
							<div className="pos-detail-grid">
								{Object.entries(position.parameters).map(([key, value]) => {
									const paramDef = productDef?.params.find((p) => p.key === key);
									return (
										<div className="pos-detail-item" key={key}>
											<span className="pos-detail-key">{paramDef?.label ?? key}</span>
											<span>{renderParamValue(paramDef, value)}</span>
										</div>
									);
								})}
							</div>
						</div>

						{/* Pricing result */}
						{r && (
							<>
								<div className="pos-detail-section">
									<h4>Pricing</h4>
									<div className="pos-detail-grid">
										<div className="pos-detail-item">
											<span className="pos-detail-key">NPV</span>
											<span className={r.npv >= 0 ? "profit" : "loss"}>{fmtCurrency(r.npv)}</span>
										</div>
										<div className="pos-detail-item">
											<span className="pos-detail-key">Unit Price</span>
											<span>{fmtCurrency(r.unit_price)}</span>
										</div>
										<div className="pos-detail-item">
											<span className="pos-detail-key">Engine</span>
											<span className="pos-engine">{r.engine || "—"}</span>
										</div>
										<div className="pos-detail-item">
											<span className="pos-detail-key">Priced At</span>
											<span>{r.priced_at ? new Date(r.priced_at).toLocaleString() : "—"}</span>
										</div>
										{r.mc_std_error > 0 && r.mc_std_error < 1e100 && (
											<div className="pos-detail-item">
												<span className="pos-detail-key">MC Std Error</span>
												<span>{fmt(r.mc_std_error, 8)}</span>
											</div>
										)}
										{r.diagnostics && (
											<div className="pos-detail-item full-width">
												<span className="pos-detail-key">Diagnostics</span>
												<span className="pos-detail-diag">{r.diagnostics}</span>
											</div>
										)}
									</div>
								</div>

								{/* Greeks */}
								<div className="pos-detail-section">
									<h4>Greeks</h4>
									<div className="pos-detail-grid greeks-grid">
										<div className="pos-detail-item greek-card">
											<span className="pos-detail-key">Delta (Δ)</span>
											<span className="greek-value">{fmt(r.greeks.delta)}</span>
										</div>
										<div className="pos-detail-item greek-card">
											<span className="pos-detail-key">Gamma (Γ)</span>
											<span className="greek-value">{fmt(r.greeks.gamma)}</span>
										</div>
										<div className="pos-detail-item greek-card">
											<span className="pos-detail-key">Vega (ν)</span>
											<span className="greek-value">{fmt(r.greeks.vega)}</span>
										</div>
										<div className="pos-detail-item greek-card">
											<span className="pos-detail-key">Theta (θ)</span>
											<span className="greek-value">{fmt(r.greeks.theta)}</span>
										</div>
										<div className="pos-detail-item greek-card">
											<span className="pos-detail-key">Rho (ρ)</span>
											<span className="greek-value">{fmt(r.greeks.rho)}</span>
										</div>
									</div>
								</div>
							</>
						)}

						{!r && (
							<div className="pos-detail-section">
								<p className="empty-msg">Not yet priced. Click "Price All" to compute.</p>
							</div>
						)}
					</div>
				)}
			</div>
		</div>
	);
}
