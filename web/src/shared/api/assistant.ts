import { useQuery } from "@tanstack/react-query";
import { api } from "./client";
import { ApiError } from "./errors";
import type { components } from "./schema.gen";

export type AssistantStatus = components["schemas"]["AssistantStatusResponse"];

/**
 * Whether the assistant's model server answers, and with which model. Asked
 * again every half minute while the page is open: the server is shared with
 * other work on the same GPU and can be stopped or switched to another model
 * at any time. A failed request is its own state — the API is unreachable,
 * which says nothing about the model server.
 */
export function useAssistantStatus() {
	return useQuery<AssistantStatus, ApiError>({
		queryKey: ["assistant", "status"],
		refetchInterval: 30_000,
		staleTime: 15_000,
		retry: false,
		queryFn: async ({ signal }) => {
			const { data, error } = await api.GET("/api/assistant/status", {
				signal,
			});
			if (error !== undefined) throw ApiError.from(error);
			return data;
		},
	});
}

export type ScriptingChatRequest =
	components["schemas"]["ScriptingChatRequest"];

/** One `data:` payload of the assistant's event stream — the shape is
 * documented by api/app/assistant/schemas.py (ScriptingChatEvent). */
export type AssistantEvent =
	| { type: "delta"; text: string }
	/** The draft was rejected by the parser; a new one follows. */
	| { type: "retry"; attempt: number; error: string }
	| {
			type: "script";
			script: string;
			valid: boolean;
			error: string | null;
	  }
	| { type: "error"; message: string }
	| { type: "done" };

/**
 * Stream one assistant turn. `api` (not a bare fetch) so the bearer token,
 * base URL and ApiError normalisation apply — a 429 "assistant busy" throws
 * an ApiError before the first event. openapi-fetch cannot type an SSE body,
 * hence `parseAs: "stream"` and the small parser below.
 */
export async function* streamScriptingChat(
	body: ScriptingChatRequest,
	signal: AbortSignal,
): AsyncGenerator<AssistantEvent> {
	const { data } = await api.POST("/api/assistant/scripting/chat", {
		body,
		signal,
		parseAs: "stream",
	});
	if (!data) return;
	const reader = (data as ReadableStream<Uint8Array>).getReader();
	const decoder = new TextDecoder("utf-8");
	let buffer = "";
	try {
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			buffer += decoder.decode(value, { stream: true });
			// SSE events end with a blank line; a chunk may hold several or half of one.
			let cut: number;
			while ((cut = buffer.indexOf("\n\n")) !== -1) {
				const block = buffer.slice(0, cut);
				buffer = buffer.slice(cut + 2);
				const line = block.split("\n").find((l) => l.startsWith("data:"));
				if (line) yield JSON.parse(line.slice(5).trim()) as AssistantEvent;
			}
		}
	} finally {
		void reader.cancel().catch(() => undefined);
	}
}
