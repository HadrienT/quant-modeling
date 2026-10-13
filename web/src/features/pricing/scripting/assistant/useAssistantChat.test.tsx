import { act, renderHook, waitFor } from "@testing-library/react";
import { http, HttpResponse } from "msw";
import { afterEach, describe, expect, it } from "vitest";
import { server } from "@/shared/test/msw-server";
import { useAssistantChat } from "./useAssistantChat";

afterEach(() => server.resetHandlers());

const context = () => ({ day_count: "ACT/365F" as const });

function answer(events: object[]) {
	const wire = events.map((e) => `data: ${JSON.stringify(e)}\n\n`).join("");
	return new HttpResponse(wire, {
		headers: { "Content-Type": "text/event-stream" },
	});
}

describe("useAssistantChat, not signed in", () => {
	it("shows the free questions left, then asks to sign in", async () => {
		let calls = 0;
		server.use(
			http.post("*/api/assistant/scripting/chat", () =>
				++calls === 1
					? answer([
							{ type: "quota", remaining: 1 },
							{ type: "delta", text: "Here." },
							{ type: "done" },
						])
					: HttpResponse.json(
							{
								code: "anonymous_quota_exhausted",
								message:
									"You have used today's free questions. Sign in to continue.",
							},
							{ status: 429 },
						),
			),
		);
		const { result } = renderHook(() => useAssistantChat(context));
		expect(result.current.remaining).toBeNull();

		await act(() => result.current.send("a call"));
		await waitFor(() => expect(result.current.remaining).toBe(1));
		expect(result.current.messages.at(-1)?.content).toBe("Here.");

		await act(() => result.current.send("a put"));
		await waitFor(() => expect(result.current.remaining).toBe(0));
		const last = result.current.messages.at(-1);
		expect(last?.signIn).toBe(true);
		expect(last?.error).toMatch(/Sign in to continue/);
		// The conversation already on screen stays.
		expect(result.current.messages[1]?.content).toBe("Here.");
	});

	it("a busy assistant is not a sign-in prompt", async () => {
		server.use(
			http.post("*/api/assistant/scripting/chat", () =>
				HttpResponse.json(
					{ code: "rate_limited", message: "The assistant is busy." },
					{ status: 429 },
				),
			),
		);
		const { result } = renderHook(() => useAssistantChat(context));
		await act(() => result.current.send("a call"));
		await waitFor(() => expect(result.current.busy).toBe(false));
		expect(result.current.messages.at(-1)?.signIn).toBeUndefined();
		expect(result.current.remaining).toBeNull();
	});
});
