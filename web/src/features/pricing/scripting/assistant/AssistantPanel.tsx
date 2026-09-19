import { useEffect, useRef, useState } from "react";
import { Button } from "@/shared/ui";
import { MessageBody } from "./MessageBody";
import { useAssistantChat, type EditorContext } from "./useAssistantChat";

const STARTERS = [
	"Explique mon script étape par étape.",
	"Écris un call asiatique, fixings mensuels sur 6 mois, strike 100.",
	"Quelles sont les limites du langage ?",
];

/**
 * Chat with the scripting assistant (api/app/assistant). It sees the script in
 * the editor and the error shown next to it; a script it writes has already
 * passed the real parser and is loaded into the editor only on request.
 */
export function AssistantPanel({
	script,
	error,
	valuationDate,
	dayCount,
	onScript,
}: {
	script: string;
	error: string | null;
	valuationDate: string;
	dayCount: EditorContext["day_count"];
	onScript: (script: string) => void;
}) {
	// The context is read at send time, not render time: the user may edit the
	// script between two messages and the assistant must see the latest one.
	const ctx = useRef<EditorContext>({ day_count: dayCount });
	ctx.current = {
		current_script: script || null,
		last_error: error,
		valuation_date: valuationDate,
		day_count: dayCount,
	};
	const chat = useAssistantChat(() => ctx.current);
	const [draft, setDraft] = useState("");
	const bottom = useRef<HTMLDivElement>(null);
	const scriptRef = useRef(script);
	scriptRef.current = script;

	useEffect(() => {
		bottom.current?.scrollIntoView?.({ block: "end" });
	}, [chat.messages]);

	const submit = (text: string) => {
		if (!text.trim() || chat.busy) return;
		setDraft("");
		void chat.send(text);
	};
	// Overwrites the editor; hands back what was there so the bubble can undo.
	const apply = (next: string) => {
		const before = scriptRef.current;
		onScript(next);
		return before;
	};

	return (
		<div className="flex flex-col gap-3 rounded-md border border-hairline bg-surface p-4">
			<div className="flex items-center justify-between">
				<p className="text-2xs text-ink-muted uppercase">Assistant</p>
				{chat.messages.length > 0 && (
					<Button size="sm" variant="ghost" onClick={chat.reset}>
						New chat
					</Button>
				)}
			</div>
			<div className="flex max-h-[28rem] min-h-40 flex-col gap-3 overflow-y-auto">
				{chat.messages.length === 0 ? (
					<div className="flex flex-col gap-2">
						<p className="text-2xs text-ink-secondary">
							Describe a product; I write the script and check it against the
							parser. It reads your editor, so you can also ask about the script
							or the error below. I do not compute prices — use Price.
						</p>
						{STARTERS.map((s) => (
							<button
								key={s}
								type="button"
								className="rounded-sm border border-hairline px-2 py-1 text-left text-2xs text-ink-secondary hover:bg-surface-raised"
								onClick={() => submit(s)}
							>
								{s}
							</button>
						))}
					</div>
				) : (
					chat.messages.map((m) => (
						<MessageBody key={m.id} message={m} onApply={apply} />
					))
				)}
				{chat.busy && <p className="text-2xs text-ink-muted">Thinking…</p>}
				<div ref={bottom} />
			</div>
			<form
				className="flex flex-col gap-2"
				onSubmit={(e) => {
					e.preventDefault();
					submit(draft);
				}}
			>
				<textarea
					aria-label="Message to the assistant"
					className="min-h-16 rounded border border-hairline bg-surface px-2 py-1 text-sm"
					placeholder="Ask about the language, or describe a product…"
					value={draft}
					onChange={(e) => setDraft(e.target.value)}
					onKeyDown={(e) => {
						if (e.key === "Enter" && !e.shiftKey) {
							e.preventDefault();
							submit(draft);
						}
					}}
				/>
				<div className="flex gap-2">
					{chat.busy ? (
						<Button
							type="button"
							variant="secondary"
							size="sm"
							onClick={chat.stop}
						>
							Stop
						</Button>
					) : (
						<Button type="submit" size="sm" disabled={!draft.trim()}>
							Send
						</Button>
					)}
				</div>
			</form>
		</div>
	);
}
