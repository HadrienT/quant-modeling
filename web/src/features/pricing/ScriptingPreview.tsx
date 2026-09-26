import { useMemo, useState } from "react";
import { usePricing } from "@/shared/api";
import { Button, Field } from "@/shared/ui";
import { MetricRowSkeleton } from "@/shared/ui/states";
import { ScriptingMarketFields, type DayCount } from "./ScriptingMarketFields";
import { alignToValuationDate } from "./scripting/alignDates";
import { DEFAULT_PRODUCT } from "./scripting/library";
import { LibraryPicker } from "./scripting/LibraryPicker";
import { ScriptEditor } from "./scripting/ScriptEditor";
import { ScriptRejected } from "./scripting/ScriptRejected";
import { AssistantSidebar } from "./scripting/assistant/AssistantSidebar";
import { useValidateScript } from "./scripting/useValidateScript";
import { parseScriptDiagnostic } from "./scripting/parseScriptDiagnostic";
import { ValidationSummary } from "./scripting/ValidationSummary";
import { ModelChoice, type ScriptModel } from "./scripting/ModelChoice";
import { ModelDecision } from "./scripting/ModelDecision";
import { ModelWarnings } from "./scripting/ModelWarnings";
import { ScriptResult } from "./scripting/ScriptResult";
import {
	countUnderlyings,
	underlyingsRequest,
	useUnderlyings,
} from "./scripting/underlyings";
import { UnderlyingsFields } from "./scripting/UnderlyingsFields";

/**
 * Preview surface for the payoff scripting language (blueprint/wp/16-scripting.md).
 * Deliberately standalone — WP 16 §8.4 — it does NOT go through the WP 07
 * product catalog. Its product library (scripting/library/*.qms) writes the
 * structured products of Bouzoubaa & Osseiran as scripts.
 *
 * Endpoints: POST /price/scripted, POST /price/scripted/validate. Regenerate
 * schema.gen.ts after an API change: `python scripts/gen_openapi.py
 * web/openapi.json && cd web && npm run api:types:local`.
 */
const today = () => new Date().toISOString().slice(0, 10);

export default function ScriptingPreview() {
	const [product, setProduct] = useState(DEFAULT_PRODUCT);
	const [script, setScript] = useState(() =>
		alignToValuationDate(DEFAULT_PRODUCT.script, today()),
	);
	const [spot, setSpot] = useState("100");
	const [ratePct, setRatePct] = useState("3");
	const [divPct, setDivPct] = useState("0");
	const [volPct, setVolPct] = useState("20");
	const [valuationDate, setValuationDate] = useState(today);
	const [dayCount, setDayCount] = useState<DayCount>("ACT/365F");
	const [fuzzy, setFuzzy] = useState(false);
	const [defaultEps, setDefaultEps] = useState("1");
	const [sampler, setSampler] = useState<"pseudo" | "sobol">("pseudo");
	const [nPaths, setNPaths] = useState("200000");
	const [seed, setSeed] = useState("1");
	const [model, setModel] = useState<ScriptModel>("auto");
	const [ticker, setTicker] = useState("SPY");
	const [request, setRequest] = useState<Record<string, unknown> | null>(null);
	const underlyings = useUnderlyings();
	const n = useMemo(() => countUnderlyings(script), [script]);
	const market = model !== "black_scholes";

	const price = usePricing({
		endpoint: request ? "/price/scripted" : null,
		body: request,
		enabled: Boolean(request),
	});
	const validate = useValidateScript();

	const r = price.data;
	const error = price.error ?? validate.error;
	const diagnostic = useMemo(
		() => (error ? parseScriptDiagnostic(error.message) : null),
		[error],
	);
	const single = market
		? { ticker }
		: {
				spot: Number(spot),
				dividend: Number(divPct) / 100,
				vol: Number(volPct) / 100,
			};

	return (
		<div className="mx-auto flex max-w-5xl flex-col gap-6">
			<header className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">Payoff scripting</h1>
				<p className="text-sm text-ink-secondary">
					Describe a product in text, or start from the library; it prices
					through the same generic Monte-Carlo engine as every other product —
					no dedicated payoff code.
				</p>
			</header>

			<div className="grid grid-cols-1 gap-6 lg:grid-cols-[minmax(0,1fr)_380px]">
				<form
					className="flex flex-col gap-4 rounded-md border border-hairline bg-surface p-4"
					onSubmit={(e) => {
						e.preventDefault();
						setRequest({
							script,
							model,
							...(n > 1 ? underlyingsRequest(n, market, underlyings) : single),
							rate: Number(ratePct) / 100,
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
					<LibraryPicker
						product={product}
						onPick={(p) => {
							setProduct(p);
							setScript(alignToValuationDate(p.script, valuationDate));
						}}
					/>

					<ScriptEditor
						value={script}
						onChange={setScript}
						diagnostic={diagnostic}
					/>

					<ModelChoice
						model={model}
						onModel={setModel}
						ticker={ticker}
						onTicker={setTicker}
						recommendation={validate.data?.recommendation}
						multi={n > 1}
					/>

					{n > 1 && <UnderlyingsFields n={n} market={market} u={underlyings} />}

					<ScriptingMarketFields
						marketDriven={market || n > 1}
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
							Fuzzy (smoothed comparisons, for digitals and barriers)
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

				<AssistantSidebar
					script={script}
					error={error?.message ?? null}
					valuationDate={valuationDate}
					dayCount={dayCount}
					onScript={setScript}
				/>
			</div>

			{price.isLoading && <MetricRowSkeleton />}
			{error && <ScriptRejected message={error.message} />}
			{r && <ScriptResult result={r} />}
			{r?.model_choice && <ModelDecision choice={r.model_choice} />}
			{r && <ModelWarnings warnings={r.warnings ?? []} />}
		</div>
	);
}
