import { type InputHTMLAttributes, useId } from "react";
import { Input } from "./primitives";
import { Label } from "./primitives";
import { cn } from "./cn";

/** Labelled input — a properly associated label + control, used across the forms. */
export function Field({
	label,
	hint,
	className,
	...props
}: { label: string; hint?: string } & InputHTMLAttributes<HTMLInputElement>) {
	const id = useId();
	return (
		<div className={cn("flex flex-col gap-1", className)}>
			<Label htmlFor={id}>{label}</Label>
			<Input id={id} {...props} />
			{hint && <span className="text-2xs text-ink-muted">{hint}</span>}
		</div>
	);
}
