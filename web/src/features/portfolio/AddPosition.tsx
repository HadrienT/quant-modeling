import { useState } from "react";
import type { Position } from "@/shared/api";
import { CATALOG, CATEGORY_LABELS, ParamForm } from "@/shared/products";
import { Button, Input, Label } from "@/shared/ui";

/** Reuses the shared product catalog + form — no portfolio-specific catalog (WP 09 §2). */
export function AddPosition({ onAdd }: { onAdd: (p: Position) => void }) {
	const enabled = CATALOG.filter((p) => p.enabled);
	const [key, setKey] = useState(enabled[0]!.key);
	const descriptor = enabled.find((p) => p.key === key)!;
	const [values, setValues] = useState<Record<string, unknown>>(
		descriptor.defaults,
	);
	const [direction, setDirection] = useState<"long" | "short">("long");
	const [quantity, setQuantity] = useState(1);
	const [entry, setEntry] = useState(0);

	return (
		<div className="flex flex-col gap-3 rounded-md border border-hairline bg-surface p-4">
			<div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
				<label className="flex flex-col gap-1">
					<Label>Product</Label>
					<select
						className="h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
						value={key}
						onChange={(e) => {
							setKey(e.target.value);
							const d = enabled.find((p) => p.key === e.target.value)!;
							setValues(d.defaults);
						}}
					>
						{enabled.map((p) => (
							<option key={p.key} value={p.key}>
								{CATEGORY_LABELS[p.category]} · {p.label}
							</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Direction</Label>
					<select
						className="h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
						value={direction}
						onChange={(e) => setDirection(e.target.value as "long" | "short")}
					>
						<option value="long">long</option>
						<option value="short">short</option>
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Quantity</Label>
					<Input
						type="number"
						value={quantity}
						onChange={(e) => setQuantity(Number(e.target.value))}
					/>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Entry price</Label>
					<Input
						type="number"
						value={entry}
						onChange={(e) => setEntry(Number(e.target.value))}
					/>
				</label>
			</div>

			<ParamForm
				key={key}
				descriptor={descriptor}
				values={values}
				onChange={setValues}
			/>

			<div>
				<Button
					size="sm"
					onClick={() =>
						onAdd({
							id: crypto.randomUUID?.() ?? Math.random().toString(36).slice(2),
							label: `${descriptor.label} ${direction}`,
							product_type: descriptor.key as never,
							category: descriptor.category as never,
							direction,
							quantity,
							entry_price: entry,
							parameters: values as never,
							result: null,
						})
					}
				>
					Add to portfolio
				</Button>
			</div>
		</div>
	);
}
