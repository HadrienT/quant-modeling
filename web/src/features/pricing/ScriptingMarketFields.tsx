import { Field } from "@/shared/ui";

const DAY_COUNTS = ["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] as const;
export type DayCount = (typeof DAY_COUNTS)[number];

/** The flat Black-Scholes market inputs + MC settings shared by ScriptingPreview. */
export function ScriptingMarketFields(props: {
	valuationDate: string;
	onValuationDate: (v: string) => void;
	spot: string;
	onSpot: (v: string) => void;
	ratePct: string;
	onRatePct: (v: string) => void;
	divPct: string;
	onDivPct: (v: string) => void;
	volPct: string;
	onVolPct: (v: string) => void;
	dayCount: DayCount;
	onDayCount: (v: DayCount) => void;
	sampler: "pseudo" | "sobol";
	onSampler: (v: "pseudo" | "sobol") => void;
	nPaths: string;
	onNPaths: (v: string) => void;
	seed: string;
	onSeed: (v: string) => void;
}) {
	return (
		<div className="grid grid-cols-2 gap-4 sm:grid-cols-3">
			<Field
				label="Valuation date"
				type="date"
				value={props.valuationDate}
				onChange={(e) => props.onValuationDate(e.target.value)}
			/>
			<Field
				label="Spot"
				inputMode="decimal"
				value={props.spot}
				onChange={(e) => props.onSpot(e.target.value)}
			/>
			<Field
				label="Rate %"
				inputMode="decimal"
				value={props.ratePct}
				onChange={(e) => props.onRatePct(e.target.value)}
			/>
			<Field
				label="Dividend %"
				inputMode="decimal"
				value={props.divPct}
				onChange={(e) => props.onDivPct(e.target.value)}
			/>
			<Field
				label="Vol %"
				inputMode="decimal"
				value={props.volPct}
				onChange={(e) => props.onVolPct(e.target.value)}
			/>
			<label className="flex flex-col gap-1 text-sm">
				<span className="text-2xs text-ink-muted uppercase">Day count</span>
				<select
					className="rounded border border-hairline bg-surface px-2 py-1"
					value={props.dayCount}
					onChange={(e) => props.onDayCount(e.target.value as DayCount)}
				>
					{DAY_COUNTS.map((d) => (
						<option key={d} value={d}>
							{d}
						</option>
					))}
				</select>
			</label>
			<Field
				label="Paths"
				inputMode="numeric"
				value={props.nPaths}
				onChange={(e) => props.onNPaths(e.target.value)}
			/>
			<Field
				label="Seed"
				inputMode="numeric"
				value={props.seed}
				onChange={(e) => props.onSeed(e.target.value)}
			/>
			<label className="flex flex-col gap-1 text-sm">
				<span className="text-2xs text-ink-muted uppercase">Sampler</span>
				<select
					className="rounded border border-hairline bg-surface px-2 py-1"
					value={props.sampler}
					onChange={(e) =>
						props.onSampler(e.target.value as "pseudo" | "sobol")
					}
				>
					<option value="pseudo">Pseudo-random</option>
					<option value="sobol">Sobol RQMC</option>
				</select>
			</label>
		</div>
	);
}
