import { useState } from "react";
import { useNavigate } from "@tanstack/react-router";
import { useDeleteAccount } from "@/shared/api";
import { useSessionActions } from "@/shared/session";
import {
	Button,
	Dialog,
	DialogContent,
	DialogDescription,
	DialogHeader,
	DialogTitle,
	Input,
	toast,
} from "@/shared/ui";

const CONFIRM = "delete";

/**
 * Account erasure. Typing a word guards against a stray click: there is no
 * undo, the server keeps no copy. Browser storage (local portfolios, presets,
 * theme) is the visitor's own, so clearing it is offered, on by default.
 */
export function DeleteAccountDialog({
	open,
	name,
	onOpenChange,
}: {
	open: boolean;
	name: string;
	onOpenChange: (open: boolean) => void;
}) {
	const [typed, setTyped] = useState("");
	const [clearBrowser, setClearBrowser] = useState(true);
	const del = useDeleteAccount();
	const { logout } = useSessionActions();
	const navigate = useNavigate();

	const confirm = () =>
		del.mutate(undefined, {
			onSuccess: () => {
				if (clearBrowser) {
					try {
						localStorage.clear();
					} catch {
						// storage blocked: nothing of ours is in it then
					}
				}
				logout();
				onOpenChange(false);
				toast.success("Account deleted");
				void navigate({ to: "/" });
			},
			onError: (e) => toast.error(e.message),
		});

	return (
		<Dialog
			open={open}
			onOpenChange={(o) => {
				if (!o) setTyped("");
				onOpenChange(o);
			}}
		>
			<DialogContent>
				<DialogHeader>
					<DialogTitle>Delete {name}?</DialogTitle>
					<DialogDescription>
						The account and every portfolio saved to it are erased from the
						server now. This cannot be undone — download your data first if you
						want to keep it.
					</DialogDescription>
				</DialogHeader>
				<label className="mt-4 flex flex-col gap-1 text-xs text-ink-secondary">
					Type <span className="font-mono text-ink">{CONFIRM}</span> to confirm
					<Input
						value={typed}
						onChange={(e) => setTyped(e.target.value)}
						autoComplete="off"
					/>
				</label>
				<label className="mt-3 flex items-center gap-2 text-xs text-ink-secondary">
					<input
						type="checkbox"
						checked={clearBrowser}
						onChange={(e) => setClearBrowser(e.target.checked)}
					/>
					Also clear what this site stored in this browser (local portfolios,
					presets, theme)
				</label>
				<div className="mt-4 flex justify-end gap-2">
					<Button size="sm" variant="ghost" onClick={() => onOpenChange(false)}>
						Keep it
					</Button>
					<Button
						size="sm"
						variant="danger"
						disabled={typed.trim().toLowerCase() !== CONFIRM || del.isPending}
						onClick={confirm}
					>
						{del.isPending ? "Deleting…" : "Delete account"}
					</Button>
				</div>
			</DialogContent>
		</Dialog>
	);
}
