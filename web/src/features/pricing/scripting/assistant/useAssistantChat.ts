import { useCallback, useRef, useState } from "react";
import { ApiError, streamScriptingChat } from "@/shared/api";
import type { ScriptingChatRequest } from "@/shared/api";

export type ChatScript = {
	script: string;
	valid: boolean;
	error: string | null;
};

export type ChatMessage = {
	id: number;
	role: "user" | "assistant";
	content: string;
	/** Assistant only: the script the reply carries, once the parser has seen it. */
	script?: ChatScript;
	/** Assistant only: set while the parser-rejected draft is being rewritten. */
	repairing?: boolean;
	/** Assistant only: the request failed (model server down, assistant busy…). */
	error?: string;
	/** Assistant only: the failure is lifted by signing in (free questions used up). */
	signIn?: boolean;
};

/** What the editor holds right now — sent with every turn so the assistant
 * talks about the user's actual script and the error they are looking at. */
export type EditorContext = Pick<
	ScriptingChatRequest,
	"current_script" | "last_error" | "valuation_date" | "day_count"
>;

const MAX_HISTORY = 20;
/** Error code of the 429 that ends a visitor's free questions (api routers/assistant.py). */
const QUOTA_EXHAUSTED = "anonymous_quota_exhausted";

export function useAssistantChat(getContext: () => EditorContext) {
	const [messages, setMessages] = useState<ChatMessage[]>([]);
	const [busy, setBusy] = useState(false);
	// Free questions left today; null when signed in or before the first answer.
	const [remaining, setRemaining] = useState<number | null>(null);
	const abort = useRef<AbortController | null>(null);
	const nextId = useRef(1);
	// Latest transcript for building the request without a stale closure.
	const transcript = useRef<ChatMessage[]>([]);
	transcript.current = messages;

	const patchLast = useCallback((patch: (m: ChatMessage) => ChatMessage) => {
		// `reset()` can empty the list while a stream is still winding down.
		setMessages((all) => {
			const last = all[all.length - 1];
			return last ? [...all.slice(0, -1), patch(last)] : all;
		});
	}, []);

	const send = useCallback(
		async (text: string) => {
			const content = text.trim();
			if (!content || abort.current) return;
			const history = [
				...transcript.current.filter((m) => !m.error),
				{ id: 0, role: "user" as const, content },
			].slice(-MAX_HISTORY);
			setMessages((all) => [
				...all,
				{ id: nextId.current++, role: "user", content },
				{ id: nextId.current++, role: "assistant", content: "" },
			]);
			const ctrl = new AbortController();
			abort.current = ctrl;
			setBusy(true);
			// Only a visitor's stream opens with a `quota` event.
			let left: number | null = null;
			try {
				const request: ScriptingChatRequest = {
					messages: history.map(({ role, content: c }) => ({
						role,
						content: c,
					})),
					...getContext(),
				};
				for await (const ev of streamScriptingChat(request, ctrl.signal)) {
					if (ev.type === "quota") {
						left = ev.remaining;
					} else if (ev.type === "delta") {
						patchLast((m) => ({
							...m,
							content: m.content + ev.text,
							repairing: false,
						}));
					} else if (ev.type === "retry") {
						patchLast((m) => ({ ...m, content: "", repairing: true }));
					} else if (ev.type === "script") {
						const { script, valid, error } = ev;
						patchLast((m) => ({ ...m, script: { script, valid, error } }));
					} else if (ev.type === "error") {
						patchLast((m) => ({ ...m, error: ev.message }));
					}
				}
			} catch (e) {
				const err = ApiError.from(e);
				if (err.code === QUOTA_EXHAUSTED) {
					left = 0;
					patchLast((m) => ({ ...m, error: err.message, signIn: true }));
				} else if (err.kind !== "abort") {
					patchLast((m) => ({ ...m, error: err.message }));
				}
			} finally {
				setRemaining(left);
				abort.current = null;
				setBusy(false);
				patchLast((m) => ({ ...m, repairing: false }));
			}
		},
		[getContext, patchLast],
	);

	const stop = useCallback(() => abort.current?.abort(), []);
	const reset = useCallback(() => {
		abort.current?.abort();
		setMessages([]);
	}, []);

	return { messages, busy, remaining, send, stop, reset };
}
