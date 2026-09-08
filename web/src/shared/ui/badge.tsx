import { type VariantProps, cva } from "class-variance-authority";
import { type HTMLAttributes } from "react";
import { cn } from "./cn";

const badge = cva(
	"inline-flex items-center gap-1 rounded-xs px-1.5 py-0.5 text-2xs font-medium",
	{
		variants: {
			tone: {
				neutral: "bg-surface-raised text-ink-secondary",
				good: "bg-[color-mix(in_srgb,var(--color-good)_18%,transparent)] text-good",
				warning:
					"bg-[color-mix(in_srgb,var(--color-warning)_18%,transparent)] text-warning",
				serious:
					"bg-[color-mix(in_srgb,var(--color-serious)_18%,transparent)] text-serious",
				critical:
					"bg-[color-mix(in_srgb,var(--color-critical)_18%,transparent)] text-critical",
				accent:
					"bg-[color-mix(in_srgb,var(--color-accent)_18%,transparent)] text-accent",
			},
		},
		defaultVariants: { tone: "neutral" },
	},
);

export type BadgeProps = HTMLAttributes<HTMLSpanElement> &
	VariantProps<typeof badge>;

export function Badge({ className, tone, ...props }: BadgeProps) {
	return <span className={cn(badge({ tone }), className)} {...props} />;
}
