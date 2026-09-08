import { type ReactNode } from "react";
import { AlertTriangle, Inbox, LogIn, SearchX } from "lucide-react";
import { ApiError } from "@/shared/api/errors";
import { Button } from "./button";
import { Skeleton } from "./primitives";
import { cn } from "./cn";

/**
 * Shared loading / empty / error states — blueprint WP 03 §5.
 * Skeletons take the SHAPE of the content so the page does not jump when data
 * lands. Empty distinguishes "no results" / "nothing yet" / "sign in".
 */

/* ── Loading skeletons ─────────────────────────────────────────────────── */
export function TableSkeleton({ rows = 8 }: { rows?: number }) {
	return (
		<div className="flex flex-col gap-2" aria-busy="true" aria-live="polite">
			<Skeleton className="h-8 w-full" />
			{Array.from({ length: rows }, (_, i) => (
				<Skeleton key={i} className="h-6 w-full" />
			))}
		</div>
	);
}

export function ChartSkeleton({ className }: { className?: string }) {
	return (
		<Skeleton
			className={cn("aspect-[16/9] w-full", className)}
			aria-busy="true"
		/>
	);
}

export function MetricRowSkeleton() {
	return (
		<div className="grid grid-cols-2 gap-4 rounded-md border border-hairline bg-surface p-4 lg:grid-cols-5">
			{Array.from({ length: 5 }, (_, i) => (
				<div key={i} className="flex flex-col gap-2">
					<Skeleton className="h-3 w-16" />
					<Skeleton className="h-6 w-24" />
				</div>
			))}
		</div>
	);
}

/* ── Empty ─────────────────────────────────────────────────────────────── */
type EmptyKind = "no-results" | "nothing-yet" | "sign-in";

export function EmptyState({
	kind = "nothing-yet",
	title,
	description,
	action,
}: {
	kind?: EmptyKind;
	title: string;
	description?: string;
	action?: ReactNode;
}) {
	const Icon =
		kind === "no-results" ? SearchX : kind === "sign-in" ? LogIn : Inbox;
	return (
		<div className="flex flex-col items-center gap-2 rounded-md border border-dashed border-hairline bg-surface px-6 py-12 text-center">
			<Icon className="size-6 text-ink-muted" />
			<p className="text-sm font-medium text-ink">{title}</p>
			{description && (
				<p className="max-w-sm text-xs text-ink-secondary">{description}</p>
			)}
			{action && <div className="mt-2">{action}</div>}
		</div>
	);
}

/* ── Error ─────────────────────────────────────────────────────────────── */
export function ErrorState({
	error,
	onRetry,
	compact = false,
}: {
	error: unknown;
	onRetry?: () => void;
	compact?: boolean;
}) {
	const apiError = error instanceof ApiError ? error : ApiError.from(error);
	return (
		<div
			role="alert"
			className={cn(
				"flex flex-col gap-2 rounded-md border border-critical/40 bg-critical/5 p-4",
				compact && "p-3",
			)}
		>
			<div className="flex items-center gap-2 text-critical">
				<AlertTriangle className="size-4" />
				<span className="text-sm font-medium">{apiError.message}</span>
			</div>
			{onRetry && apiError.kind !== "abort" && (
				<div>
					<Button size="sm" variant="secondary" onClick={onRetry}>
						Retry
					</Button>
				</div>
			)}
			{apiError.detail != null && (
				<details className="text-2xs text-ink-muted">
					<summary className="cursor-pointer select-none">
						Technical detail
					</summary>
					<pre className="mt-1 max-h-40 overflow-auto font-mono whitespace-pre-wrap">
						{typeof apiError.detail === "string"
							? apiError.detail
							: JSON.stringify(apiError.detail, null, 2)}
					</pre>
				</details>
			)}
		</div>
	);
}
