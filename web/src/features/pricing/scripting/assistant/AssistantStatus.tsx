import { useAssistantStatus } from "@/shared/api";
import { Tooltip, TooltipContent, TooltipTrigger, cn } from "@/shared/ui";

type Look = { dot: string; label: string; detail: string };

/** What each state looks like: a colour is never the only signal, the label
 * says it too. */
function look(q: ReturnType<typeof useAssistantStatus>): Look {
	if (q.isLoading)
		return {
			dot: "bg-ink-muted animate-pulse",
			label: "Checking",
			detail: "Asking whether the model answers.",
		};
	if (!q.data)
		return {
			dot: "bg-ink-muted",
			label: "Unknown",
			detail: "The API did not answer, so the model could not be asked.",
		};
	const { state, detail, model, configured } = q.data;
	if (state === "up")
		return {
			dot: "bg-good",
			label: "Online",
			detail: `${detail} Model: ${model}.`,
		};
	if (!configured)
		return { dot: "bg-ink-muted", label: "Not configured", detail };
	return { dot: "bg-critical", label: "Offline", detail };
}

/**
 * Whether the assistant's model answers: a dot and a word, with the detail on
 * hover and, when it is not simply online, written out under the header.
 * `compact` is the dot alone, for the tab that opens the assistant.
 */
export function AssistantStatus({ compact = false }: { compact?: boolean }) {
	const q = useAssistantStatus();
	const { dot, label, detail } = look(q);
	const dotEl = (
		<span aria-hidden="true" className={cn("size-2 rounded-full", dot)} />
	);
	if (compact)
		return (
			<span className="inline-flex items-center" title={`Assistant: ${label}`}>
				{dotEl}
				<span className="sr-only">({label})</span>
			</span>
		);
	return (
		<Tooltip>
			<TooltipTrigger asChild>
				<span
					role="status"
					aria-label={`Assistant status: ${label}`}
					className="inline-flex items-center gap-1.5 text-2xs text-ink-secondary"
				>
					{dotEl}
					{label}
				</span>
			</TooltipTrigger>
			<TooltipContent>{detail}</TooltipContent>
		</Tooltip>
	);
}

/** The sentence shown in the panel when the assistant is not simply online. */
export function AssistantStatusNotice() {
	const q = useAssistantStatus();
	if (!q.data || q.data.state === "up") return null;
	return (
		<p
			role="alert"
			className={cn(
				"rounded-sm border p-2 text-2xs",
				q.data.configured
					? "border-critical/40 bg-critical/5 text-critical"
					: "border-hairline bg-surface-raised text-ink-secondary",
			)}
		>
			{q.data.detail}
		</p>
	);
}
