import { Slot } from "@radix-ui/react-slot";
import { type VariantProps, cva } from "class-variance-authority";
import { type ButtonHTMLAttributes, forwardRef } from "react";
import { cn } from "./cn";

const button = cva(
	"inline-flex items-center justify-center gap-2 whitespace-nowrap rounded-sm text-sm font-medium transition-colors disabled:pointer-events-none disabled:opacity-50 [&_svg]:size-4 [&_svg]:shrink-0",
	{
		variants: {
			variant: {
				primary: "bg-accent text-accent-ink hover:brightness-110",
				secondary:
					"border border-hairline bg-surface-raised text-ink hover:bg-surface",
				ghost: "text-ink-secondary hover:bg-surface-raised hover:text-ink",
				danger: "bg-critical text-accent-ink hover:brightness-110",
				link: "text-accent underline-offset-4 hover:underline",
			},
			size: {
				sm: "h-7 px-2.5 text-xs",
				md: "h-9 px-3.5",
				lg: "h-10 px-5",
				icon: "size-9",
			},
		},
		defaultVariants: { variant: "primary", size: "md" },
	},
);

export type ButtonProps = ButtonHTMLAttributes<HTMLButtonElement> &
	VariantProps<typeof button> & { asChild?: boolean };

export const Button = forwardRef<HTMLButtonElement, ButtonProps>(
	({ className, variant, size, asChild = false, type, ...props }, ref) => {
		const Comp = asChild ? Slot : "button";
		return (
			<Comp
				ref={ref}
				className={cn(button({ variant, size }), className)}
				type={asChild ? undefined : (type ?? "button")}
				{...props}
			/>
		);
	},
);
Button.displayName = "Button";

export { button as buttonVariants };
