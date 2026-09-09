import { useMemo, useState } from "react";
import { usePricing } from "@/shared/api";
import type { PricingResponse } from "@/shared/api";
import { Button, Field } from "@/shared/ui";
import { Uncertainty } from "@/shared/ui/density";
import { ErrorState, MetricRowSkeleton } from "@/shared/ui/states";

/**
 * Preview surface for the dated Monte-Carlo path (timeline + calendar / day-count
 * architecture). Deliberately standalone — it does NOT go through the WP 07
 * product catalog yet; it exists so the new engine can be exercised from the
 * browser while the catalog integration is designed.
 *
 * Endpoint: POST /price/option/dated-asian (regenerate schema.gen.ts after the
 * API change: `python scripts/gen_openapi.py web/openapi.json && cd web &&
 * npm run api:types:local`).
 */

const DAY_COUNTS = ["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] as const;

function monthlySchedule(from: string, months: number): string {
	const base = new Date(from);
	if (Number.isNaN(base.getTime())) return "";
	const out: string[] = [];
	for (let i = 1; i <= months; i += 1) {
		const d = new Date(base);
		d.setMonth(d.getMonth() + i);
		out.push(d.toISOString().slice(0, 10));
	}
	return out.join("\n");
}

export default function DatedAsianPreview() {
	const [valuationDate, setValuationDate] = useState("2025-01-02");
	const [spot, setSpot] = useState("100");
	const [strike, setStrike] = useState("100");
	const [ratePct, setRatePct] = useState("5");
	const [divPct, setDivPct] = useState("0");
	const [volPct, setVolPct] = useState("25");
	const [isCall, setIsCall] = useState(true);
	const [geometric, setGeometric] = useState(false);
	const [dayCount, setDayCount] =
		useState<(typeof DAY_COUNTS)[number]>("ACT/365F");
	const [sampler, setSampler] = useState<"pseudo" | "sobol">("pseudo");
	const [nPaths, setNPaths] = useState("200000");
	const [seed, setSeed] = useState("1");
	const [fixingsText, setFixingsText] = useState(() =>
		monthlySchedule("2025-01-02", 12),
	);
	// Snapshot of the params at the last "Price" click — the query re-runs only
	// when this changes, not on every keystroke.
	const [request, setRequest] = useState<Record<string, unknown> | null>(null);

	const fixingDates = useMemo(
		() =>
			fixingsText
				.split(/\s+/)
				.map((s) => s.trim())
				.filter(Boolean),
		[fixingsText],
	);

	const q = usePricing({
		endpoint: request ? "/price/option/dated-asian" : null,
		body: request,
		enabled: Boolean(request),
	});

	const r = q.data as PricingResponse | undefined;

	return (
		<div className="mx-auto flex max-w-3xl flex-col gap-6">
			<header className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">Dated Asian — preview</h1>
				<p className="text-sm text-ink-secondary">
					Average-price Asian priced from calendar fixing dates through the
					timeline simulation engine. Standalone preview of the date / day-count
					layer, outside the product catalog.
				</p>
			</header>

			<form
				className="grid grid-cols-2 gap-4 rounded-md border border-hairline bg-surface p-4 sm:grid-cols-3"
				onSubmit={(e) => {
					e.preventDefault();
					setRequest({
						spot: Number(spot),
						rate: Number(ratePct) / 100,
						dividend: Number(divPct) / 100,
						vol: Number(volPct) / 100,
						valuation_date: valuationDate,
						fixing_dates: fixingDates,
						strike: Number(strike),
						is_call: isCall,
						geometric,
						day_count: dayCount,
						sampler,
						n_paths: Number(nPaths),
						seed: Number(seed),
					});
				}}
			>
				<Field
					label="Valuation date"
					type="date"
					value={valuationDate}
					onChange={(e) => setValuationDate(e.target.value)}
				/>
				<Field
					label="Spot"
					inputMode="decimal"
					value={spot}
					onChange={(e) => setSpot(e.target.value)}
				/>
				<Field
					label="Strike"
					inputMode="decimal"
					value={strike}
					onChange={(e) => setStrike(e.target.value)}
				/>
				<Field
					label="Rate %"
					inputMode="decimal"
					value={ratePct}
					onChange={(e) => setRatePct(e.target.value)}
				/>
				<Field
					label="Dividend %"
					inputMode="decimal"
					value={divPct}
					onChange={(e) => setDivPct(e.target.value)}
				/>
				<Field
					label="Vol %"
					inputMode="decimal"
					value={volPct}
					onChange={(e) => setVolPct(e.target.value)}
				/>
				<Field
					label="Paths"
					inputMode="numeric"
					value={nPaths}
					onChange={(e) => setNPaths(e.target.value)}
				/>
				<Field
					label="Seed"
					inputMode="numeric"
					value={seed}
					onChange={(e) => setSeed(e.target.value)}
				/>
				<label className="flex flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">Day count</span>
					<select
						className="rounded border border-hairline bg-surface px-2 py-1"
						value={dayCount}
						onChange={(e) =>
							setDayCount(e.target.value as (typeof DAY_COUNTS)[number])
						}
					>
						{DAY_COUNTS.map((d) => (
							<option key={d} value={d}>
								{d}
							</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">Sampler</span>
					<select
						className="rounded border border-hairline bg-surface px-2 py-1"
						value={sampler}
						onChange={(e) =>
							setSampler(e.target.value as "pseudo" | "sobol")
						}
					>
						<option value="pseudo">Pseudo-random</option>
						<option value="sobol">Sobol RQMC</option>
					</select>
				</label>
				<label className="flex items-center gap-2 text-sm">
					<input
						type="checkbox"
						checked={isCall}
						onChange={(e) => setIsCall(e.target.checked)}
					/>
					Call
				</label>
				<label className="flex items-center gap-2 text-sm">
					<input
						type="checkbox"
						checked={geometric}
						onChange={(e) => setGeometric(e.target.checked)}
					/>
					Geometric average
				</label>

				<div className="col-span-full flex flex-col gap-1">
					<span className="text-2xs text-ink-muted uppercase">
						Fixing dates ({fixingDates.length}) — one ISO date per line
					</span>
					<textarea
						className="h-40 rounded border border-hairline bg-surface p-2 font-mono text-2xs"
						value={fixingsText}
						onChange={(e) => setFixingsText(e.target.value)}
					/>
					<button
						type="button"
						className="self-start text-2xs text-ink-muted underline"
						onClick={() =>
							setFixingsText(monthlySchedule(valuationDate, 12))
						}
					>
						Fill: 12 monthly from valuation date
					</button>
				</div>

				<div className="col-span-full">
					<Button type="submit">Price</Button>
				</div>
			</form>

			{q.isLoading && <MetricRowSkeleton />}
			{q.error && (
				<ErrorState error={q.error} onRetry={() => q.refetch()} />
			)}
			{r && (
				<div className="rounded-md border border-hairline bg-surface p-4">
					<span className="text-2xs text-ink-muted uppercase">Present value</span>
					<div className="text-xl">
						<Uncertainty
							value={r.npv}
							stdError={r.mc_std_error}
							magnitude="price"
						/>
					</div>
					{r.diagnostics && (
						<p className="mt-2 font-mono text-2xs text-ink-muted">
							{r.diagnostics}
						</p>
					)}
				</div>
			)}
		</div>
	);
}
