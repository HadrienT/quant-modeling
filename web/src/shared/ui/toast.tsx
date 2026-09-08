import { Toaster as SonnerToaster, toast } from "sonner";

/**
 * Reserved for the results of user actions (portfolio saved, position removed,
 * backtest finished) — never for loading errors (WP 03 §6).
 */
export function Toaster() {
	return (
		<SonnerToaster
			position="bottom-right"
			toastOptions={{
				classNames: {
					toast: "!bg-surface !border-hairline !text-ink !rounded-md !text-sm",
					description: "!text-ink-secondary",
				},
			}}
		/>
	);
}

export { toast };
