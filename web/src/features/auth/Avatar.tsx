import { cn } from "@/shared/ui";
import { initial } from "./account";

/** Initial-letter avatar (the Google scope is `openid email`: no photo). */
export function Avatar({
	me,
	className,
}: {
	me: { username: string; email?: string | null };
	className?: string;
}) {
	return (
		<span
			aria-hidden="true"
			className={cn(
				"flex size-7 shrink-0 items-center justify-center rounded-full bg-accent text-xs font-semibold text-accent-ink",
				className,
			)}
		>
			{initial(me)}
		</span>
	);
}
