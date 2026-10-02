import type { SwaptionInput } from "@/shared/api";
import { Field, Label, Segmented } from "@/shared/ui";
import { SwaptionModelInputs } from "./SwaptionModelInputs";

type Model = NonNullable<SwaptionInput["model"]>;

const MODELS: { value: Model; label: string; european: boolean }[] = [
	{ value: "auto", label: "Automatic", european: false },
	{ value: "bachelier", label: "Bachelier (normal)", european: true },
	{ value: "black", label: "Black (shifted lognormal)", european: true },
	{ value: "sabr", label: "SABR (shifted)", european: true },
	{ value: "hull_white", label: "Hull-White (calibrated)", european: false },
];

const num = (s: string) => (s === "" ? null : Number(s));

/**
 * The terms of a swaption and the model it is valued under. The model is
 * chosen for the exercise by default; a Bermudan only has Hull-White.
 */
export function SwaptionFields({
	value,
	onChange,
}: {
	value: SwaptionInput;
	onChange: (next: SwaptionInput) => void;
}) {
	const set = (patch: Partial<SwaptionInput>) =>
		onChange({ ...value, ...patch });
	const bermudan = value.exercise === "bermudan";
	const model = value.model ?? "auto";
	return (
		<fieldset className="flex flex-col gap-2">
			<legend className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
				Contract
			</legend>
			<div className="flex flex-wrap gap-3">
				<Segmented
					label="Swaption type"
					options={[
						{ value: "payer", label: "Payer" },
						{ value: "receiver", label: "Receiver" },
					]}
					value={value.payer === false ? "receiver" : "payer"}
					onChange={(v) => set({ payer: v === "payer" })}
				/>
				<Segmented
					label="Exercise"
					options={[
						{ value: "european", label: "European" },
						{
							value: "bermudan",
							label: "Bermudan",
							title: "Exercisable on each annual date from the expiry",
						},
					]}
					value={bermudan ? "bermudan" : "european"}
					onChange={(v) =>
						set({
							exercise: v,
							// The single-rate models cannot price a Bermudan.
							model:
								v === "bermudan" &&
								MODELS.find((m) => m.value === model)?.european
									? "auto"
									: model,
						})
					}
				/>
			</div>
			<div className="grid grid-cols-2 gap-2">
				<Field
					label="Expiry (years)"
					type="number"
					step="0.5"
					min={0.25}
					value={value.expiry}
					onChange={(e) => set({ expiry: Number(e.target.value) })}
				/>
				<Field
					label="Swap tenor (years)"
					type="number"
					step="1"
					min={1}
					value={value.tenor}
					onChange={(e) => set({ tenor: Number(e.target.value) })}
				/>
				<Field
					label="Strike (%, blank: ATM)"
					type="number"
					step="0.01"
					value={
						value.strike == null ? "" : +(value.strike * 100).toPrecision(10)
					}
					onChange={(e) => {
						const v = num(e.target.value);
						set({ strike: v == null ? null : v / 100 });
					}}
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
			<div className="flex flex-col gap-1">
				<Label htmlFor="swaption-model">Model</Label>
				<select
					id="swaption-model"
					className="h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
					value={model}
					onChange={(e) => set({ model: e.target.value as Model })}
				>
					{MODELS.map((m) => (
						<option
							key={m.value}
							value={m.value}
							disabled={bermudan && m.european}
						>
							{m.label}
						</option>
					))}
				</select>
				<span className="text-2xs text-ink-muted">
					Automatic picks the model for the exercise and says why.
				</span>
			</div>
			<SwaptionModelInputs value={value} onChange={set} />
		</fieldset>
	);
}
