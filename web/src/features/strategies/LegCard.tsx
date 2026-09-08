import { Trash2 } from "lucide-react";
import type { Leg } from "@/shared/payoff";
import { Input, cn } from "@/shared/ui";

export function LegCard({
	leg,
	onChange,
	onRemove,
}: {
	leg: Leg;
	onChange: (l: Leg) => void;
	onRemove: () => void;
}) {
	return (
		<div className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-2.5">
			<div className="flex items-center gap-1.5">
				<button
					type="button"
					className={cn(
						"rounded-xs px-1.5 py-0.5 text-2xs font-medium",
						leg.direction === "long"
							? "bg-pnl-up/15 text-pnl-up"
							: "bg-pnl-down/15 text-pnl-down",
					)}
					onClick={() =>
						onChange({
							...leg,
							direction: leg.direction === "long" ? "short" : "long",
						})
					}
				>
					{leg.direction}
				</button>
				<select
					className="h-7 rounded-sm border border-hairline bg-surface px-1 text-xs text-ink"
					value={leg.kind}
					onChange={(e) =>
						onChange({ ...leg, kind: e.target.value as Leg["kind"] })
					}
				>
					{["call", "put", "underlying", "forward"].map((k) => (
						<option key={k}>{k}</option>
					))}
				</select>
				<button
					type="button"
					aria-label="Remove leg"
					className="ml-auto text-ink-muted hover:text-critical"
					onClick={onRemove}
				>
					<Trash2 className="size-3.5" />
				</button>
			</div>
			<div className="grid grid-cols-2 gap-1.5 text-xs">
				<Field
					label="Qty"
					value={leg.quantity}
					onChange={(v) => onChange({ ...leg, quantity: v })}
				/>
				{leg.kind !== "underlying" && (
					<Field
						label="Strike"
						value={leg.strike}
						onChange={(v) => onChange({ ...leg, strike: v })}
					/>
				)}
				<Field
					label="Maturity (y)"
					value={leg.maturity}
					step={0.05}
					onChange={(v) => onChange({ ...leg, maturity: v })}
				/>
				<Field
					label="Premium"
					value={leg.premium}
					step={0.1}
					onChange={(v) => onChange({ ...leg, premium: v })}
				/>
			</div>
		</div>
	);
}

function Field({
	label,
	value,
	step,
	onChange,
}: {
	label: string;
	value: number;
	step?: number;
	onChange: (v: number) => void;
}) {
	return (
		<label className="flex flex-col gap-0.5">
			<span className="text-2xs text-ink-muted">{label}</span>
			<Input
				type="number"
				step={step}
				value={value}
				onChange={(e) => onChange(Number(e.target.value))}
				className="h-7"
			/>
		</label>
	);
}
