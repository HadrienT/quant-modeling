import { type ReactNode } from "react";
import {
	type Magnitude,
	ageSeconds,
	formatNumber,
	formatRelative,
	formatSigned,
} from "@/shared/format";
import { cn } from "./cn";

/**
 * Numeric-density primitives — blueprint WP 01 §6.
 * These, more than the buttons, give the product its instrument character.
 */

/* ── NumberCell ────────────────────────────────────────────────────────── */
export function NumberCell({
	value,
	magnitude = "plain",
	signed = false,
	className,
	title,
}: {
	value: number | null | undefined;
	magnitude?: Magnitude;
	signed?: boolean;
	className?: string;
	title?: string;
}) {
	const text = signed
		? formatSigned(value, magnitude)
		: formatNumber(value, magnitude);
	return (
		<span
			className={cn(
				"text-right font-mono text-sm text-ink tabular-nums",
				className,
			)}
			title={title}
		>
			{text}
		</span>
	);
}

/* ── Uncertainty ───────────────────────────────────────────────────────── */
export function Uncertainty({
	value,
	stdError,
	magnitude = "price",
	relativeWarnAt = 0.05,
	className,
}: {
	value: number | null | undefined;
	stdError: number | null | undefined;
	magnitude?: Magnitude;
	/** Relative std error above which the display switches to a warning. */
	relativeWarnAt?: number;
	className?: string;
}) {
	if (value == null || Number.isNaN(value)) {
		return <span className="font-mono text-sm text-ink-muted">—</span>;
	}
	const hasError = stdError != null && !Number.isNaN(stdError) && stdError > 0;
	const relative = hasError && value !== 0 ? Math.abs(stdError / value) : 0;
	const noisy = hasError && relative >= relativeWarnAt;
	return (
		<span
			className={cn(
				"font-mono text-sm whitespace-nowrap tabular-nums",
				noisy ? "text-warning" : "text-ink",
				className,
			)}
			title={
				noisy
					? `Relative standard error ${(relative * 100).toFixed(1)}% ≥ ${(
							relativeWarnAt * 100
						).toFixed(0)}%`
					: undefined
			}
		>
			{formatNumber(value, magnitude)}
			{hasError && (
				<span className="text-ink-muted">
					{" "}
					± {formatNumber(stdError, magnitude)}
				</span>
			)}
		</span>
	);
}

/* ── DeltaBadge ────────────────────────────────────────────────────────── */
export function DeltaBadge({
	value,
	magnitude = "plain",
	className,
}: {
	value: number | null | undefined;
	magnitude?: Magnitude;
	className?: string;
}) {
	if (value == null || Number.isNaN(value)) {
		return <span className="font-mono text-sm text-ink-muted">—</span>;
	}
	const up = value > 0;
	const flat = value === 0;
	return (
		<span
			className={cn(
				"inline-flex items-center gap-1 font-mono text-sm tabular-nums",
				flat ? "text-ink-muted" : up ? "text-pnl-up" : "text-pnl-down",
				className,
			)}
		>
			<span aria-hidden="true">{flat ? "→" : up ? "▲" : "▼"}</span>
			{formatSigned(value, magnitude)}
		</span>
	);
}

/* ── Metric / MetricRow ────────────────────────────────────────────────── */
export function Metric({
	label,
	value,
	unit,
	change,
	footnote,
	className,
}: {
	label: string;
	value: ReactNode;
	unit?: string;
	change?: ReactNode;
	footnote?: ReactNode;
	className?: string;
}) {
	return (
		<div className={cn("flex flex-col gap-1", className)}>
			<span className="text-2xs font-medium tracking-wide text-ink-muted uppercase">
				{label}
			</span>
			<span className="flex items-baseline gap-1.5">
				<span className="font-mono text-xl text-ink">{value}</span>
				{unit && <span className="text-xs text-ink-secondary">{unit}</span>}
			</span>
			{change && <span className="text-xs">{change}</span>}
			{footnote && <span className="text-2xs text-ink-muted">{footnote}</span>}
		</div>
	);
}

export function MetricRow({
	children,
	className,
}: {
	children: ReactNode;
	className?: string;
}) {
	return (
		<div
			className={cn(
				"grid gap-4 rounded-md border border-hairline bg-surface p-4 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-5",
				className,
			)}
		>
			{children}
		</div>
	);
}

/* ── Freshness ─────────────────────────────────────────────────────────── */
export function Freshness({
	at,
	label = "calculated",
	staleAfterSeconds = 300,
	now,
	className,
}: {
	at: string | number | Date | null | undefined;
	label?: string;
	staleAfterSeconds?: number;
	now?: number;
	className?: string;
}) {
	const age = ageSeconds(at, now);
	const stale = age != null && age > staleAfterSeconds;
	return (
		<span
			className={cn(
				"inline-flex items-center gap-1 text-2xs",
				stale ? "text-warning" : "text-ink-muted",
				className,
			)}
			title={stale ? "Older than the staleness threshold" : undefined}
		>
			{stale && <span aria-hidden="true">⚠</span>}
			{label} {formatRelative(at, now)}
		</span>
	);
}

/* ── Provenance ────────────────────────────────────────────────────────── */
export function Provenance({
	source,
	className,
}: {
	source: "market" | "manual";
	className?: string;
}) {
	const market = source === "market";
	return (
		<span
			className={cn(
				"inline-flex items-center gap-1 rounded-xs border px-1.5 py-0.5 text-2xs font-medium",
				market
					? "border-series-3 text-series-3"
					: "border-hairline text-ink-secondary",
				className,
			)}
		>
			<span aria-hidden="true">{market ? "◆" : "✎"}</span>
			{market ? "market" : "manual"}
		</span>
	);
}

/* ── EngineTag ─────────────────────────────────────────────────────────── */
const ENGINE_LABELS: Record<string, string> = {
	analytic: "analytic",
	mc: "Monte-Carlo",
	monte_carlo: "Monte-Carlo",
	pde: "PDE",
	binomial: "binomial",
	tree: "tree",
};

export function EngineTag({
	engine,
	className,
}: {
	engine: string;
	className?: string;
}) {
	return (
		<span
			className={cn(
				"inline-flex items-center rounded-xs bg-surface-raised px-1.5 py-0.5 text-2xs font-medium text-ink-secondary",
				className,
			)}
		>
			{ENGINE_LABELS[engine] ?? engine}
		</span>
	);
}
