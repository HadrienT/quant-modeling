import { formatDuration } from "@/shared/format";
import { cn } from "./cn";

/** The server's phases of one pricing (PricingResponse.timings). */
export type ComputeTimings = {
	market_ms: number;
	calibration_ms: number;
	engine_ms: number;
	other_ms: number;
	greeks_ms?: number;
};

type Segment = {
	key: string;
	label: string;
	ms: number;
	swatch: string;
	help: string;
};

/**
 * Where a pricing's server time went: market data read from the database,
 * the model's calibration, the engine alone (the Monte-Carlo run, or the
 * closed form) and the rest. The phases are exclusive, so the bar's segments
 * add up to the compute time; the greeks by bump come on top of it.
 */
export function ComputeBreakdown({
	timings,
	engine,
	className,
}: {
	timings: ComputeTimings | null | undefined;
	/** The request's engine ("mc", "analytic"…), which names the engine phase. */
	engine?: string | null;
	className?: string;
}) {
	if (!timings) return null;
	const segments: Segment[] = [
		{
			key: "market",
			label: "Market data",
			ms: timings.market_ms,
			swatch: "bg-series-1",
			help: "Reading spot, option chain, dividend or curves from the database, and preparing them.",
		},
		{
			key: "calibration",
			label: "Calibration",
			ms: timings.calibration_ms,
			swatch: "bg-series-4",
			help: "Fitting the model to that data (vol surface, Heston / SLV leverage, Hull-White). Near zero when the server's cache already holds it.",
		},
		{
			key: "engine",
			label: engineLabelOf(engine),
			ms: timings.engine_ms,
			swatch: "bg-series-3",
			help: "The numerical method alone, its adjoint greeks included. The first run of a product shape on the GPU also compiles its kernel here.",
		},
		{
			key: "other",
			label: "Other",
			ms: timings.other_ms,
			swatch: "bg-ink-muted",
			help: "Script parsing, model choice and server glue.",
		},
	];
	const total = segments.reduce((s, x) => s + x.ms, 0);
	if (total <= 0) return null;
	// The engine is always listed (it is the question this answers); the
	// others only when they took a measurable time.
	const listed = segments.filter((s) => s.key === "engine" || s.ms >= 0.05);
	const greeks = timings.greeks_ms ?? 0;
	const summary = listed
		.map((s) => `${s.label} ${formatDuration(s.ms)}`)
		.join(", ");

	return (
		<div className={cn("mt-2 flex flex-col gap-1", className)}>
			<div
				role="img"
				aria-label={`Server time ${formatDuration(total)}: ${summary}`}
				className="flex h-1.5 w-full overflow-hidden rounded-xs bg-surface-raised"
			>
				{segments.map((s) =>
					s.ms > 0 ? (
						<div
							key={s.key}
							className={cn("h-full", s.swatch)}
							style={{ width: `${(100 * s.ms) / total}%` }}
							title={`${s.label}: ${formatDuration(s.ms)}`}
						/>
					) : null,
				)}
			</div>
			<ul className="flex flex-wrap gap-x-3 gap-y-0.5 font-mono text-2xs text-ink-muted tabular-nums">
				{listed.map((s) => (
					<li key={s.key} className="flex items-center gap-1" title={s.help}>
						<span
							aria-hidden
							className={cn("inline-block size-1.5 rounded-full", s.swatch)}
						/>
						<span className="text-ink-secondary">{s.label}</span>
						{formatDuration(s.ms)}
					</li>
				))}
				{greeks >= 0.05 && (
					<li
						className="flex items-center gap-1"
						title="The greeks the engine does not give, by bumping an input and revaluing. Not in the compute time."
					>
						<span className="text-ink-secondary">+ greeks by bump</span>
						{formatDuration(greeks)}
					</li>
				)}
			</ul>
		</div>
	);
}

/** The engine phase's name for a request's `engine` field. */
function engineLabelOf(engine: string | null | undefined): string {
	switch (engine) {
		case "mc":
			return "Monte Carlo";
		case "analytic":
			return "Closed form";
		case "pde":
			return "PDE";
		case "binomial":
		case "trinomial":
			return "Tree";
		default:
			return "Engine";
	}
}
