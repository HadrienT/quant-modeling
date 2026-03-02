import type { Leg, LegType, LegSide } from "./types";
import { LEG_TYPE_LABELS } from "./types";

type Props = {
	leg: Leg;
	color: string;
	onUpdate: (leg: Leg) => void;
	onRemove: () => void;
};

export default function LegCard({ leg, color, onUpdate, onRemove }: Props) {
	const isDigital = leg.type === "digital-call" || leg.type === "digital-put";
	const isStock = leg.type === "stock";

	const set = <K extends keyof Leg>(key: K, val: Leg[K]) =>
		onUpdate({ ...leg, [key]: val });

	return (
		<div className="leg-card" style={{ borderTopColor: color }}>
			<div className="leg-card-header">
				<span className="leg-card-color" style={{ background: color }} />
				<input
					className="leg-card-label"
					value={leg.label}
					onChange={(e) => set("label", e.target.value)}
				/>
				<button className="leg-card-remove" onClick={onRemove} title="Remove leg">
					×
				</button>
			</div>

			<div className="leg-card-fields">
				<label className="leg-card-field">
					<span>Type</span>
					<select
						value={leg.type}
						onChange={(e) => set("type", e.target.value as LegType)}
					>
						{Object.entries(LEG_TYPE_LABELS).map(([v, l]) => (
							<option key={v} value={v}>
								{l}
							</option>
						))}
					</select>
				</label>

				<label className="leg-card-field">
					<span>Side</span>
					<select
						value={leg.side}
						onChange={(e) => set("side", e.target.value as LegSide)}
					>
						<option value="long">Long</option>
						<option value="short">Short</option>
					</select>
				</label>

				<label className="leg-card-field">
					<span>{isStock ? "Entry" : "Strike"}</span>
					<input
						type="number"
						value={leg.strike}
						onChange={(e) => set("strike", Number(e.target.value))}
					/>
				</label>

				{!isStock && (
					<label className="leg-card-field">
						<span>Premium</span>
						<input
							type="number"
							step="0.1"
							value={leg.premium}
							onChange={(e) => set("premium", Number(e.target.value))}
						/>
					</label>
				)}

				<label className="leg-card-field">
					<span>Qty</span>
					<input
						type="number"
						min={1}
						value={leg.quantity}
						onChange={(e) => set("quantity", Math.max(1, Number(e.target.value)))}
					/>
				</label>

				{isDigital && (
					<label className="leg-card-field">
						<span>Cash</span>
						<input
							type="number"
							step="0.1"
							value={leg.cashAmount}
							onChange={(e) => set("cashAmount", Number(e.target.value))}
						/>
					</label>
				)}
			</div>
		</div>
	);
}
