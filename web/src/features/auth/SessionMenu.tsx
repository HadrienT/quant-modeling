import { LogOut, UserRound } from "lucide-react";
import { Button } from "@/shared/ui";
import { useMe, useSessionActions } from "@/shared/session";

/** Session control in the nav (WP 03 §2 / WP 04). */
export function SessionMenu() {
	const me = useMe();
	const { logout, setDialogOpen } = useSessionActions();

	if (me.isLoading) return <div className="h-9 w-16" aria-hidden="true" />;

	if (me.data?.username) {
		return (
			<div className="flex items-center gap-1.5 pl-1">
				<span className="flex items-center gap-1 text-xs text-ink-secondary">
					<UserRound className="size-3.5" />
					{me.data.username}
				</span>
				<Button
					variant="ghost"
					size="icon"
					aria-label="Log out"
					onClick={logout}
				>
					<LogOut className="size-4" />
				</Button>
			</div>
		);
	}

	return (
		<Button size="sm" variant="secondary" onClick={() => setDialogOpen(true)}>
			Sign in
		</Button>
	);
}
