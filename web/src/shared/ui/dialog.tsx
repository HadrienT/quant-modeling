import {
	type ComponentPropsWithoutRef,
	type ComponentRef,
	type HTMLAttributes,
	forwardRef,
} from "react";
import * as DialogPrimitive from "@radix-ui/react-dialog";
import { X } from "lucide-react";
import { cn } from "./cn";

export const Dialog = DialogPrimitive.Root;
export const DialogTrigger = DialogPrimitive.Trigger;
export const DialogClose = DialogPrimitive.Close;
export const DialogPortal = DialogPrimitive.Portal;

export const DialogOverlay = forwardRef<
	ComponentRef<typeof DialogPrimitive.Overlay>,
	ComponentPropsWithoutRef<typeof DialogPrimitive.Overlay>
>(({ className, ...props }, ref) => (
	<DialogPrimitive.Overlay
		ref={ref}
		className={cn(
			"fixed inset-0 z-50 bg-[color-mix(in_srgb,var(--color-canvas)_72%,transparent)]",
			className,
		)}
		{...props}
	/>
));
DialogOverlay.displayName = "DialogOverlay";

export const DialogContent = forwardRef<
	ComponentRef<typeof DialogPrimitive.Content>,
	ComponentPropsWithoutRef<typeof DialogPrimitive.Content>
>(({ className, children, ...props }, ref) => (
	<DialogPortal>
		<DialogOverlay />
		<DialogPrimitive.Content
			ref={ref}
			className={cn(
				"fixed top-1/2 left-1/2 z-50 w-full max-w-md -translate-x-1/2 -translate-y-1/2 rounded-lg border border-hairline bg-surface p-5 shadow-xl focus:outline-none",
				className,
			)}
			{...props}
		>
			{children}
			<DialogPrimitive.Close
				className="absolute top-3 right-3 rounded-xs p-1 text-ink-muted transition-colors hover:text-ink"
				aria-label="Close"
			>
				<X className="size-4" />
			</DialogPrimitive.Close>
		</DialogPrimitive.Content>
	</DialogPortal>
));
DialogContent.displayName = "DialogContent";

export function DialogHeader({
	className,
	...props
}: HTMLAttributes<HTMLDivElement>) {
	return (
		<div className={cn("mb-4 flex flex-col gap-1", className)} {...props} />
	);
}

export const DialogTitle = forwardRef<
	ComponentRef<typeof DialogPrimitive.Title>,
	ComponentPropsWithoutRef<typeof DialogPrimitive.Title>
>(({ className, ...props }, ref) => (
	<DialogPrimitive.Title
		ref={ref}
		className={cn("text-lg font-semibold text-ink", className)}
		{...props}
	/>
));
DialogTitle.displayName = "DialogTitle";

export const DialogDescription = forwardRef<
	ComponentRef<typeof DialogPrimitive.Description>,
	ComponentPropsWithoutRef<typeof DialogPrimitive.Description>
>(({ className, ...props }, ref) => (
	<DialogPrimitive.Description
		ref={ref}
		className={cn("text-sm text-ink-secondary", className)}
		{...props}
	/>
));
DialogDescription.displayName = "DialogDescription";
