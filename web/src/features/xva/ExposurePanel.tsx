import { useState } from "react";
import type { XvaResponse } from "@/shared/api";
import { Segmented } from "@/shared/ui";
import { ProfileChart, type ProfileLine } from "@/shared/viz";
import { Explained, Figure } from "./Explained";
import { ExposureSurface } from "./ExposureSurface";
import { amount, plusMinus } from "./format";
import { EPE_TEX, EXPOSURE_TEX, GREGORY } from "./texts";

/** The fan's ranges of the value, from the widest to the narrowest. */
const FAN: [number, number][] = [
	[0.01, 0.99],
	[0.05, 0.95],
	[0.2, 0.8],
];

/**
 * The exposure of the netting set through time: the expected exposures and
 * the potential future exposure over a fan of the quantiles of its value,
 * with the same set without its collateral dashed behind when there is a
 * CSA. The measure is a choice, shown as one: prices integrate the
 * risk-neutral profile, limits and capital read the real-world one.
 */
export function ExposurePanel({ data }: { data: XvaResponse }) {
	const [measure, setMeasure] = useState<"pricing" | "risk">("pricing");
	const e = data.exposure;
	const profile = measure === "pricing" ? e : data.risk;
	const open = data.exposure_uncollateralised;
	const peak = profile.pfe.indexOf(Math.max(...profile.pfe));

	const lines: ProfileLine[] = [
		{ label: "EE", values: profile.ee },
		{ label: "ENE", values: profile.ene },
		{ label: "PFE", values: profile.pfe },
	];
	if (measure === "pricing" && open && open.times.length === e.times.length)
		lines.push({ label: "EE without the CSA", values: open.ee, dashed: true });

	// The fan: nested ranges of the value, from the widest to the narrowest.
	const bands =
		measure === "pricing"
			? FAN.flatMap(([lo, hi]) => {
					const low = e.quantile_levels.indexOf(lo);
					const high = e.quantile_levels.indexOf(hi);
					if (low < 0 || high < 0) return [];
					return {
						label: `${lo * 100} % to ${hi * 100} % of the value`,
						lower: e.value_quantiles[low]!,
						upper: e.value_quantiles[high]!,
					};
				})
			: undefined;

	return (
		<div className="flex flex-col gap-4">
			<div className="flex flex-wrap items-center justify-between gap-3">
				<div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
					<Figure label="Value today" value={amount(data.value_today)} />
					<Figure
						label="EPE"
						value={amount(profile.epe)}
						note={plusMinus(profile.epe_error)}
					/>
					<Figure
						label="Effective EPE"
						value={amount(profile.eepe)}
						note={plusMinus(profile.eepe_error)}
					/>
					<Figure
						label="Peak PFE"
						value={amount(profile.pfe[peak])}
						note={plusMinus(profile.pfe_error[peak])}
					/>
				</div>
				<Segmented
					label="Measure"
					options={[
						{
							value: "pricing",
							label: "Pricing measure",
							title: "Risk-neutral scenarios: what the adjustments integrate",
						},
						{
							value: "risk",
							label: "Real-world measure",
							title:
								"Scenarios drawn from how rates have moved: what limits and capital read",
						},
					]}
					value={measure}
					onChange={setMeasure}
				/>
			</div>
			<ProfileChart
				title="Exposure profile"
				times={profile.times}
				lines={lines}
				bands={bands}
				bandsLabel="1–99 %, 5–95 % and 20–80 % of the value"
			/>
			{measure === "pricing" && <ExposureSurface exposure={e} />}
			<Explained title="Exposure" tex={EXPOSURE_TEX} source={GREGORY}>
				V(t) is what the netting set is worth to the bank at a future date. Only
				its positive part is lost if the counterparty defaults: its average is
				the expected exposure. Its negative part is what the bank owes, and
				matters for the bank&apos;s own default. The potential future exposure
				is a high quantile, the figure a credit limit is set against.
			</Explained>
			<Explained title="EPE and Effective EPE" tex={EPE_TEX} source={GREGORY}>
				One number for a profile: its average over the life of the trades. The
				regulatory version only looks one year ahead and never lets the profile
				come down, because short trades are rolled over.
			</Explained>
		</div>
	);
}
