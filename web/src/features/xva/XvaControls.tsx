import type { XvaPortfolios, XvaRating } from "@/shared/api";
import {
	Segmented,
	Tooltip,
	TooltipContent,
	TooltipTrigger,
} from "@/shared/ui";
import { FUNDING_BP, PATHS, type XvaView, isOffered } from "./state";

/**
 * What the netting set is and under what terms it is held: the portfolio, the
 * credit of both parties, the collateral agreement, the funding spread, and
 * the wrong-way scenario — offered as buttons, each saying on hover what it
 * assumes (the parameter has no market quote to read it from).
 */
export function XvaControls({
	options,
	view,
	onChange,
}: {
	options: XvaPortfolios;
	view: XvaView;
	onChange: (patch: Partial<XvaView>) => void;
}) {
	const rating = (
		label: string,
		value: XvaRating,
		set: (r: XvaRating) => void,
	) => (
		<label className="flex items-center gap-2 text-xs text-ink-secondary">
			{label}
			<select
				value={value}
				onChange={(e) => set(e.target.value as XvaRating)}
				className="h-7 rounded-sm border border-hairline bg-surface px-2 text-xs text-ink"
			>
				{options.ratings.map((r) => (
					<option key={r} value={r}>
						{r}
					</option>
				))}
			</select>
		</label>
	);

	return (
		<div className="flex flex-col gap-3 rounded-sm border border-hairline bg-surface p-3">
			<div className="flex flex-col gap-1.5">
				<span className="text-xs font-medium text-ink-secondary">
					Portfolio
				</span>
				<Segmented
					label="Portfolio"
					options={options.portfolios
						.filter((p) => isOffered(p.id))
						.map((p) => ({ value: p.id, label: p.label, title: p.lesson }))}
					value={view.portfolio}
					onChange={(portfolio) => onChange({ portfolio })}
				/>
			</div>
			<div className="flex flex-wrap items-center gap-x-6 gap-y-3">
				{rating("Counterparty", view.counterparty, (counterparty) =>
					onChange({ counterparty }),
				)}
				{rating("Bank", view.own, (own) => onChange({ own }))}
				<div className="flex items-center gap-2 text-xs text-ink-secondary">
					<span>Collateral</span>
					<Segmented
						label="Collateral agreement"
						options={[
							{ value: "none", label: "None" },
							{
								value: "vm",
								label: "Variation margin",
								title:
									"Zero threshold, daily calls, 10-day margin period of risk",
							},
							{
								value: "im",
								label: "+ initial margin",
								title: "Both parties also post initial margin, held apart",
							},
						]}
						value={view.csa}
						onChange={(csa) => onChange({ csa })}
					/>
				</div>
				<div className="flex items-center gap-2 text-xs text-ink-secondary">
					<span>Funding spread</span>
					<Segmented
						label="Funding spread"
						options={FUNDING_BP.map((b) => ({ value: b, label: `${b} bp` }))}
						value={view.funding}
						onChange={(funding) => onChange({ funding })}
					/>
				</div>
				<div className="flex items-center gap-2 text-xs text-ink-secondary">
					<span>Paths</span>
					<Segmented
						label="Simulated paths"
						options={PATHS.map((n) => ({
							value: n,
							label: n.toLocaleString("en-US"),
						}))}
						value={view.paths}
						onChange={(paths) => onChange({ paths })}
					/>
				</div>
			</div>
			<div className="flex flex-wrap items-center gap-2 text-xs text-ink-secondary">
				<span>Wrong-way risk</span>
				<div
					role="group"
					aria-label="Wrong-way risk scenario"
					className="flex flex-wrap gap-1"
				>
					{options.wrong_way_scenarios.map((s) => (
						<Tooltip key={s.id}>
							<TooltipTrigger asChild>
								<button
									type="button"
									aria-pressed={s.wrong_way_risk === view.wwr}
									onClick={() => onChange({ wwr: s.wrong_way_risk })}
									className={
										"rounded-sm border px-2 py-1 text-xs " +
										(s.wrong_way_risk === view.wwr
											? "border-accent text-ink"
											: "border-hairline text-ink-secondary")
									}
								>
									{s.label}
								</button>
							</TooltipTrigger>
							<TooltipContent
								side="bottom"
								className="max-w-sm leading-relaxed"
							>
								{s.explanation}
							</TooltipContent>
						</Tooltip>
					))}
				</div>
			</div>
		</div>
	);
}
