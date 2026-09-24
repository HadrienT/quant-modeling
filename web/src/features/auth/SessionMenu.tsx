import { Link } from "@tanstack/react-router";
import { Briefcase, LogOut, UserRound } from "lucide-react";
import {
	Button,
	DropdownMenu,
	DropdownMenuContent,
	DropdownMenuItem,
	DropdownMenuLabel,
	DropdownMenuSeparator,
	DropdownMenuTrigger,
} from "@/shared/ui";
import { useMe, useSessionActions } from "@/shared/session";
import { displayName } from "./account";
import { Avatar } from "./Avatar";

/** Session control in the nav (WP 03 §2 / WP 04): avatar → account menu. */
export function SessionMenu() {
	const me = useMe();
	const { logout, setDialogOpen } = useSessionActions();

	if (me.isLoading) return <div className="h-9 w-16" aria-hidden="true" />;

	if (me.data?.username) {
		const user = me.data;
		return (
			<DropdownMenu>
				<DropdownMenuTrigger asChild>
					<button
						type="button"
						aria-label={`Account menu for ${displayName(user)}`}
						className="ml-1 rounded-full focus-visible:outline-2 focus-visible:outline-accent"
					>
						<Avatar me={user} />
					</button>
				</DropdownMenuTrigger>
				<DropdownMenuContent>
					<DropdownMenuLabel className="flex items-center gap-2.5">
						<Avatar me={user} className="size-8" />
						<span className="flex min-w-0 flex-col">
							<span className="truncate text-sm font-medium text-ink">
								{displayName(user)}
							</span>
							<span className="text-2xs text-ink-muted">
								{user.provider === "google"
									? "Signed in with Google"
									: "Password account"}
							</span>
						</span>
					</DropdownMenuLabel>
					<DropdownMenuSeparator />
					<DropdownMenuItem asChild>
						<Link to="/profile">
							<UserRound />
							Profile
						</Link>
					</DropdownMenuItem>
					<DropdownMenuItem asChild>
						<Link to="/portfolio">
							<Briefcase />
							My portfolios
						</Link>
					</DropdownMenuItem>
					<DropdownMenuSeparator />
					<DropdownMenuItem onSelect={logout}>
						<LogOut />
						Sign out
					</DropdownMenuItem>
				</DropdownMenuContent>
			</DropdownMenu>
		);
	}

	return (
		<Button size="sm" variant="secondary" onClick={() => setDialogOpen(true)}>
			Sign in
		</Button>
	);
}
