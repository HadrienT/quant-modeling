import type { SwapInput } from "@/shared/api";
import { Field, Segmented } from "@/shared/ui";

/** The terms of a vanilla swap: side, forward start, tenor, fixed rate. */
export function SwapFields({
	value,
	onChange,
}: {
	value: SwapInput;
	onChange: (next: SwapInput) => void;
}) {
	const set = (patch: Partial<SwapInput>) => onChange({ ...value, ...patch });
	return (
		<fieldset className="flex flex-col gap-2">
			<legend className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
				Contract
			</legend>
			<Segmented
				label="Side"
				options={[
					{ value: "payer", label: "Pay fixed" },
					{ value: "receiver", label: "Receive fixed" },
				]}
				value={value.payer === false ? "receiver" : "payer"}
				onChange={(v) => set({ payer: v === "payer" })}
			/>
			<div className="grid grid-cols-2 gap-2">
				<Field
					label="Start (years)"
					type="number"
					step="0.25"
					min={0}
					value={value.start}
					onChange={(e) => set({ start: Number(e.target.value) })}
				/>
				<Field
					label="Tenor (years)"
					type="number"
					step="1"
					min={1}
					value={value.tenor}
					onChange={(e) => set({ tenor: Number(e.target.value) })}
				/>
				<Field
					label="Fixed rate (%)"
					type="number"
					step="0.01"
					value={+(value.fixed_rate * 100).toPrecision(10)}
					onChange={(e) => set({ fixed_rate: Number(e.target.value) / 100 })}
				/>
				<Field
					label="Notional"
					type="number"
					step="1000000"
					min={1}
					value={value.notional}
					onChange={(e) => set({ notional: Number(e.target.value) })}
				/>
			</div>
		</fieldset>
	);
}
