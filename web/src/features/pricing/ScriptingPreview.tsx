import { useMemo, useState } from "react";
import { usePricing } from "@/shared/api";
import type { PricingResponse } from "@/shared/api";
import { Button, Field } from "@/shared/ui";
import { Uncertainty } from "@/shared/ui/density";
import { MetricRowSkeleton } from "@/shared/ui/states";
import { SCRIPTING_EXAMPLES } from "./scripting/examples";
import { ScriptingMarketFields, type DayCount } from "./ScriptingMarketFields";
import { ScriptEditor } from "./scripting/ScriptEditor";
import { LanguageReference } from "./scripting/LanguageReference";
import { useValidateScript } from "./scripting/useValidateScript";
import { parseScriptDiagnostic } from "./scripting/parseScriptDiagnostic";
import { ValidationSummary } from "./scripting/ValidationSummary";

/**
 * Preview surface for the payoff scripting language (blueprint/wp/16-scripting.md).
 * Deliberately standalone — WP 16 §8.4 — it does NOT go through the WP 07
 * product catalog; it exists so a script can be exercised from the browser
 * while the catalog integration is designed.
 *
 * Endpoints: POST /price/scripted, POST /price/scripted/validate. Regenerate
 * schema.gen.ts after an API change: `python scripts/gen_openapi.py
 * web/openapi.json && cd web && npm run api:types:local`.
 */
const DEFAULT_SCRIPT = SCRIPTING_EXAMPLES["European call"] ?? "";

export default function ScriptingPreview() {
	const [script, setScript] = useState(DEFAULT_SCRIPT);
	const [spot, setSpot] = useState("100");
	const [ratePct, setRatePct] = useState("3");
	const [divPct, setDivPct] = useState("0");
	const [volPct, setVolPct] = useState("20");
	const [valuationDate, setValuationDate] = useState(() =>
		new Date().toISOString().slice(0, 10),
	);
	const [dayCount, setDayCount] = useState<DayCount>("ACT/365F");
	const [fuzzy, setFuzzy] = useState(false);
	const [defaultEps, setDefaultEps] = useState("1");
	const [sampler, setSampler] = useState<"pseudo" | "sobol">("pseudo");
	const [nPaths, setNPaths] = useState("200000");
	const [seed, setSeed] = useState("1");
	const [request, setRequest] = useState<Record<string, unknown> | null>(null);

	const price = usePricing({
		endpoint: request ? "/price/scripted" : null,
		body: request,
		enabled: Boolean(request),
	});
	const validate = useValidateScript();

	const r = price.data as PricingResponse | undefined;
	const error = price.error ?? validate.error;
	const diagnostic = useMemo(
		() => (error ? parseScriptDiagnostic(error.message) : null),
		[error],
	);

	return (
		<div className="mx-auto flex max-w-5xl flex-col gap-6">
			<header className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">
					Payoff scripting — preview
				</h1>
				<p className="text-sm text-ink-secondary">
					Describe a product in text; it prices through the same generic
					Monte-Carlo engine as every other product — no dedicated payoff code.
				</p>
			</header>

			<div className="grid grid-cols-1 gap-6 lg:grid-cols-[minmax(0,1fr)_260px]">
				<form
					className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4"
					onSubmit={(e) => {
						e.preventDefault();
						setRequest({
							script,
							spot: Number(spot),
							rate: Number(ratePct) / 100,
							dividend: Number(divPct) / 100,
							vol: Number(volPct) / 100,
							valuation_date: valuationDate,
							day_count: dayCount,
							fuzzy,
							default_eps: Number(defaultEps),
							sampler,
							n_paths: Number(nPaths),
							seed: Number(seed),
						});
					}}
				>
					<label className="flex flex-col gap-1">
						<span className="text-2xs text-ink-muted uppercase">Example</span>
						<select
							className="rounded border border-hairline bg-surface px-2 py-1 text-sm"
							onChange={(e) => {
								const next = SCRIPTING_EXAMPLES[e.target.value];
								if (next) setScript(next);
							}}
							defaultValue="European call"
						>
							{Object.keys(SCRIPTING_EXAMPLES).map((name) => (
								<option key={name} value={name}>
									{name}
								</option>
							))}
						</select>
					</label>

					<ScriptEditor
						value={script}
						onChange={setScript}
						diagnostic={diagnostic}
					/>

					<ScriptingMarketFields
						valuationDate={valuationDate}
						onValuationDate={setValuationDate}
						spot={spot}
						onSpot={setSpot}
						ratePct={ratePct}
						onRatePct={setRatePct}
						divPct={divPct}
						onDivPct={setDivPct}
						volPct={volPct}
						onVolPct={setVolPct}
						dayCount={dayCount}
						onDayCount={setDayCount}
						sampler={sampler}
						onSampler={setSampler}
						nPaths={nPaths}
						onNPaths={setNPaths}
						seed={seed}
						onSeed={setSeed}
					/>

					<div className="flex flex-wrap items-center gap-4">
						<label className="flex items-center gap-2 text-sm">
							<input
								type="checkbox"
								checked={fuzzy}
								onChange={(e) => setFuzzy(e.target.checked)}
							/>
							Fuzzy (smoothed comparisons — WP 16c)
						</label>
						{fuzzy && (
							<Field
								label="Default eps"
								inputMode="decimal"
								value={defaultEps}
								onChange={(e) => setDefaultEps(e.target.value)}
							/>
						)}
					</div>

					<div className="flex gap-2">
						<Button
							type="button"
							variant="secondary"
							disabled={validate.isPending}
							onClick={() =>
								validate.mutate({
									script,
									valuation_date: valuationDate,
									day_count: dayCount,
								})
							}
						>
							{validate.isPending ? "Validating…" : "Validate"}
						</Button>
						<Button type="submit">Price</Button>
					</div>

					{validate.data && <ValidationSummary result={validate.data} />}
				</form>

				<LanguageReference />
			</div>

			{price.isLoading && <MetricRowSkeleton />}
			{error && (
				<div
					role="alert"
					className="rounded-md border border-critical/40 bg-critical/5 p-4"
				>
					<p className="mb-2 text-sm font-medium text-critical">
						Script rejected
					</p>
					<pre className="overflow-x-auto font-mono text-2xs whitespace-pre-wrap text-critical">
						{error.message}
					</pre>
				</div>
			)}
			{r && (
				<div className="rounded-md border border-hairline bg-surface p-4">
					<span className="text-2xs text-ink-muted uppercase">
						Present value
					</span>
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
