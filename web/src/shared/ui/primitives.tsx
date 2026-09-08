import {
	type ComponentPropsWithoutRef,
	type ComponentRef,
	type HTMLAttributes,
	type InputHTMLAttributes,
	type LabelHTMLAttributes,
	forwardRef,
} from "react";
import * as SeparatorPrimitive from "@radix-ui/react-separator";
import { cn } from "./cn";

/* ── Input ─────────────────────────────────────────────────────────────── */
export const Input = forwardRef<
	HTMLInputElement,
	InputHTMLAttributes<HTMLInputElement>
>(({ className, type = "text", ...props }, ref) => (
	<input
		ref={ref}
		type={type}
		className={cn(
			"h-9 w-full rounded-sm border border-hairline bg-surface px-3 text-sm text-ink shadow-sm transition-colors placeholder:text-ink-muted focus-visible:border-accent disabled:cursor-not-allowed disabled:opacity-50",
			type === "number" && "tabular-nums",
			className,
		)}
		{...props}
	/>
));
Input.displayName = "Input";

/* ── Label ─────────────────────────────────────────────────────────────── */
export const Label = forwardRef<
	HTMLLabelElement,
	LabelHTMLAttributes<HTMLLabelElement>
>(({ className, ...props }, ref) => (
	<label
		ref={ref}
		className={cn(
			"text-xs font-medium text-ink-secondary select-none",
			className,
		)}
		{...props}
	/>
));
Label.displayName = "Label";

/* ── Skeleton ──────────────────────────────────────────────────────────── */
export function Skeleton({
	className,
	...props
}: HTMLAttributes<HTMLDivElement>) {
	return (
		<div
			className={cn("animate-pulse rounded-sm bg-surface-raised", className)}
			{...props}
		/>
	);
}

/* ── Separator ─────────────────────────────────────────────────────────── */
export const Separator = forwardRef<
	ComponentRef<typeof SeparatorPrimitive.Root>,
	ComponentPropsWithoutRef<typeof SeparatorPrimitive.Root>
>(
	(
		{ className, orientation = "horizontal", decorative = true, ...props },
		ref,
	) => (
		<SeparatorPrimitive.Root
			ref={ref}
			decorative={decorative}
			orientation={orientation}
			className={cn(
				"shrink-0 bg-hairline",
				orientation === "horizontal" ? "h-px w-full" : "h-full w-px",
				className,
			)}
			{...props}
		/>
	),
);
Separator.displayName = "Separator";

/* ── Card ──────────────────────────────────────────────────────────────── */
export function Card({ className, ...props }: HTMLAttributes<HTMLDivElement>) {
	return (
		<div
			className={cn(
				"rounded-md border border-hairline bg-surface p-4",
				className,
			)}
			{...props}
		/>
	);
}
