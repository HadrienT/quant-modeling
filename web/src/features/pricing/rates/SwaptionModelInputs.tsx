import type { SwaptionInput } from "@/shared/api";
import { Field } from "@/shared/ui";

const num = (s: string) => (s === "" ? null : Number(s));

/**
 * What each single-rate model is priced with: the Bachelier vol (blank takes
 * the nearest quoted one), the Black vol and its shift, the SABR parameters.
 * Every model that has its inputs appears in the comparison, whichever one
 * the value is under.
 */
export function SwaptionModelInputs({
	value,
	onChange,
}: {
	value: SwaptionInput;
	onChange: (patch: Partial<SwaptionInput>) => void;
}) {
	const sabr = value.sabr;
	const setSabr = (patch: Partial<NonNullable<SwaptionInput["sabr"]>>) =>
		sabr && onChange({ sabr: { ...sabr, ...patch } });
	return (
		<details
			className="text-sm"
			open={value.model === "black" || value.model === "sabr"}
		>
			<summary className="cursor-pointer text-2xs text-ink-muted">
				Vols and SABR parameters of the compared models
			</summary>
			<div className="mt-2 grid grid-cols-2 gap-2">
				<Field
					label="Normal vol (bp, blank: quoted)"
					type="number"
					step="0.5"
					value={
						value.normal_vol == null
							? ""
							: +(value.normal_vol * 1e4).toPrecision(10)
					}
					onChange={(e) => {
						const v = num(e.target.value);
						onChange({ normal_vol: v == null ? null : v / 1e4 });
					}}
				/>
				<Field
					label="Black vol (%)"
					type="number"
					step="0.5"
					value={
						value.lognormal_vol == null
							? ""
							: +(value.lognormal_vol * 100).toPrecision(10)
					}
					onChange={(e) => {
						const v = num(e.target.value);
						onChange({ lognormal_vol: v == null ? null : v / 100 });
					}}
				/>
				<Field
					label="Shift (%)"
					type="number"
					step="0.25"
					min={0}
					value={+((value.shift ?? 0) * 100).toPrecision(10)}
					onChange={(e) => onChange({ shift: Number(e.target.value) / 100 })}
				/>
				{sabr && (
					<>
						<Field
							label="SABR α"
							type="number"
							step="0.001"
							value={sabr.alpha}
							onChange={(e) => setSabr({ alpha: Number(e.target.value) })}
						/>
						<Field
							label="SABR β"
							type="number"
							step="0.1"
							min={0}
							max={1}
							value={sabr.beta}
							onChange={(e) => setSabr({ beta: Number(e.target.value) })}
						/>
						<Field
							label="SABR ρ"
							type="number"
							step="0.05"
							min={-0.99}
							max={0.99}
							value={sabr.rho}
							onChange={(e) => setSabr({ rho: Number(e.target.value) })}
						/>
						<Field
							label="SABR ν"
							type="number"
							step="0.05"
							min={0}
							value={sabr.nu}
							onChange={(e) => setSabr({ nu: Number(e.target.value) })}
						/>
					</>
				)}
			</div>
		</details>
	);
}
