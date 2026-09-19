import { useState } from "react";
import { Badge, Button, copyText } from "@/shared/ui";
import type { ChatMessage } from "./useAssistantChat";
import { splitSegments } from "./segments";

/** One chat bubble. For an assistant reply that carries a script, shows what
 * the parser said about it and offers to load it into the editor — with a
 * one-step undo, because "Apply" overwrites what the user had typed. */
export function MessageBody({
	message,
	onApply,
}: {
	message: ChatMessage;
	onApply: (script: string) => string; // returns the previous editor content
}) {
	const [previous, setPrevious] = useState<string | null>(null);
	const isUser = message.role === "user";
	const { script } = message;

	if (isUser) {
		return (
			<div className="ml-6 self-end rounded-md bg-accent/10 px-3 py-2 text-sm whitespace-pre-wrap text-ink">
				{message.content}
			</div>
		);
	}
	return (
		<div className="mr-2 flex flex-col gap-2 text-sm text-ink">
			{message.repairing && (
				<p className="text-2xs text-ink-muted">
					The parser rejected the draft — rewriting it…
				</p>
			)}
			{splitSegments(message.content).map((seg, i) =>
				seg.kind === "code" ? (
					<pre
						key={i}
						className="overflow-x-auto rounded-sm border border-hairline bg-surface-raised p-2 font-mono text-2xs"
					>
						{seg.body}
					</pre>
				) : (
					<p key={i} className="whitespace-pre-wrap">
						{seg.body.trim()}
					</p>
				),
			)}
			{message.error && (
				<p role="alert" className="text-2xs text-critical">
					{message.error}
				</p>
			)}
			{script && (
				<div className="flex flex-wrap items-center gap-2">
					{script.valid ? (
						<Badge tone="good">Syntax OK — check the logic</Badge>
					) : (
						<Badge tone="critical">Does not parse</Badge>
					)}
					{script.valid && previous === null && (
						<Button
							size="sm"
							onClick={() => setPrevious(onApply(script.script))}
						>
							Apply to editor
						</Button>
					)}
					{previous !== null && (
						<Button
							size="sm"
							variant="secondary"
							onClick={() => {
								onApply(previous);
								setPrevious(null);
							}}
						>
							Undo apply
						</Button>
					)}
					<Button
						size="sm"
						variant="ghost"
						onClick={() => void copyText(script.script)}
					>
						Copy
					</Button>
					{!script.valid && script.error && (
						<pre className="w-full font-mono text-2xs whitespace-pre-wrap text-critical">
							{script.error}
						</pre>
					)}
				</div>
			)}
		</div>
	);
}
