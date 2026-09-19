import { http, HttpResponse } from "msw";
import { afterEach, describe, expect, it } from "vitest";
import { server } from "@/shared/test/msw-server";
import { streamScriptingChat, type AssistantEvent } from "./assistant";

afterEach(() => server.resetHandlers());

const body = {
	messages: [{ role: "user" as const, content: "a call" }],
	day_count: "ACT/365F" as const,
};

/** An SSE body delivered in awkward chunks: mid-event, mid-UTF-8 character. */
function sse(chunks: string[]) {
	const enc = new TextEncoder();
	const bytes = chunks.map((c) => enc.encode(c));
	return new ReadableStream<Uint8Array>({
		start(controller) {
			for (const b of bytes) controller.enqueue(b);
			controller.close();
		},
	});
}

describe("streamScriptingChat", () => {
	it("reassembles events split across network chunks", async () => {
		const wire =
			'data: {"type":"delta","text":"Voilà — "}\n\n' +
			'data: {"type":"script","script":"2027-09-10","valid":true,"error":null}\n\n' +
			'data: {"type":"done"}\n\n';
		// Cut inside an event and inside the two-byte "à".
		const cut = wire.indexOf("à") + 1;
		const raw = new TextEncoder().encode(wire);
		const split = new TextEncoder().encode(wire.slice(0, cut)).length - 1;
		const stream = new ReadableStream<Uint8Array>({
			start(c) {
				c.enqueue(raw.slice(0, split));
				c.enqueue(raw.slice(split, split + 20));
				c.enqueue(raw.slice(split + 20));
				c.close();
			},
		});
		server.use(
			http.post(
				"*/api/assistant/scripting/chat",
				() =>
					new HttpResponse(stream, {
						headers: { "Content-Type": "text/event-stream" },
					}),
			),
		);
		const events: AssistantEvent[] = [];
		for await (const e of streamScriptingChat(
			body,
			new AbortController().signal,
		)) {
			events.push(e);
		}
		expect(events.map((e) => e.type)).toEqual(["delta", "script", "done"]);
		expect(events[0]).toEqual({ type: "delta", text: "Voilà — " });
	});

	it("throws a readable ApiError when the assistant is busy (429)", async () => {
		server.use(
			http.post("*/api/assistant/scripting/chat", () =>
				HttpResponse.json(
					{ code: "http_429", message: "The assistant is busy." },
					{ status: 429 },
				),
			),
		);
		const run = async () => {
			for await (const e of streamScriptingChat(
				body,
				new AbortController().signal,
			)) {
				void e;
			}
		};
		await expect(run()).rejects.toMatchObject({
			name: "ApiError",
			status: 429,
			message: "The assistant is busy.",
		});
	});

	it("ignores a stream that ends without a trailing blank line", async () => {
		server.use(
			http.post(
				"*/api/assistant/scripting/chat",
				() =>
					new HttpResponse(sse(['data: {"type":"done"}\n\n', 'data: {"ty']), {
						headers: { "Content-Type": "text/event-stream" },
					}),
			),
		);
		const events: AssistantEvent[] = [];
		for await (const e of streamScriptingChat(
			body,
			new AbortController().signal,
		)) {
			events.push(e);
		}
		expect(events).toEqual([{ type: "done" }]);
	});
});
