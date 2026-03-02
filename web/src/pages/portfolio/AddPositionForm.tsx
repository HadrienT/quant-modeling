import { useCallback, useMemo, useState } from "react";
import type { Position } from "../../api/client";
import { CATEGORIES, catalogFor, findProduct, type ProductCategoryKey, type ProductTypeKey } from "./catalog";

type Props = {
	onAdd: (pos: Position) => void;
	onCancel: () => void;
};

function newId() {
	return crypto.randomUUID?.() ?? Math.random().toString(36).slice(2);
}

function initDefaults(type: ProductTypeKey): Record<string, unknown> {
	const def = findProduct(type);
	if (!def) return {};
	const d: Record<string, unknown> = {};
	for (const p of def.params) d[p.key] = p.default;
	return d;
}

export default function AddPositionForm({ onAdd, onCancel }: Props) {
	const [category, setCategory] = useState<ProductCategoryKey>("vanilla");
	const [productType, setProductType] = useState<ProductTypeKey>("european-call");
	const [direction, setDirection] = useState<"long" | "short">("long");
	const [quantity, setQuantity] = useState(1);
	const [entryPrice, setEntryPrice] = useState(0);
	const [label, setLabel] = useState("European Call");
	const [params, setParams] = useState<Record<string, unknown>>(() => initDefaults("european-call"));

	const products = useMemo(() => catalogFor(category), [category]);
	const productDef = useMemo(() => findProduct(productType), [productType]);

	// When category changes, reset to first product
	const handleCategoryChange = useCallback(
		(cat: ProductCategoryKey) => {
			setCategory(cat);
			const prods = catalogFor(cat);
			if (prods.length > 0) {
				setProductType(prods[0].type);
				const defaults: Record<string, unknown> = {};
				for (const p of prods[0].params) defaults[p.key] = p.default;
				setParams(defaults);
				setLabel(prods[0].label);
			}
		},
		[]
	);

	// When product type changes, reset params
	const handleProductChange = useCallback(
		(type: ProductTypeKey) => {
			setProductType(type);
			const def = findProduct(type);
			if (def) {
				const defaults: Record<string, unknown> = {};
				for (const p of def.params) defaults[p.key] = p.default;
				setParams(defaults);
				setLabel(def.label);
			}
		},
		[]
	);

	const setParam = useCallback((key: string, value: unknown) => {
		setParams((prev) => ({ ...prev, [key]: value }));
	}, []);

	const handleSubmit = () => {
		// Process is_call / is_american from select → boolean
		const processed: Record<string, unknown> = { ...params };
		for (const [k, v] of Object.entries(processed)) {
			if (v === "true") processed[k] = true;
			else if (v === "false") processed[k] = false;
		}

		const pos: Position = {
			id: newId(),
			label: label || productDef?.label || "Position",
			product_type: productType,
			category,
			direction,
			quantity,
			entry_price: entryPrice,
			parameters: processed,
			result: null,
		};
		onAdd(pos);
	};

	return (
		<div className="add-position-overlay">
			<div className="add-position-form card">
				<div className="add-pos-header">
					<h3>Add Position</h3>
					<button className="btn-icon" type="button" onClick={onCancel}>✕</button>
				</div>

				{/* ── Category pills ── */}
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

				{/* ── Product select ── */}
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

				{/* ── Label ── */}
				<label className="field" style={{ marginBottom: 8 }}>
					Label
					<input type="text" value={label} onChange={(e) => setLabel(e.target.value)} placeholder={productDef?.label || ""} />
				</label>

				{/* ── Dynamic parameters ── */}
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

				{/* ── Actions ── */}
				<div className="add-pos-actions">
					<button type="button" className="btn-secondary" onClick={onCancel}>Cancel</button>
					<button type="button" className="btn-primary" onClick={handleSubmit}>Add Position</button>
				</div>
			</div>
		</div>
	);
}
