import { Link } from "@tanstack/react-router";
import { Briefcase, LogOut, UserRound } from "lucide-react";
import {
	DropdownMenu,
	DropdownMenuContent,
	DropdownMenuItem,
	DropdownMenuLabel,
	DropdownMenuSeparator,
	DropdownMenuTrigger,
} from "@/shared/ui/dropdown-menu";
import { displayName } from "./account";
import { Avatar } from "./Avatar";

export type AccountUser = {
	username: string;
	email?: string | null;
	provider?: "password" | "google";
};

/**
 * The signed-in account menu: avatar → name, sign-in method, Profile, My
 * portfolios, Sign out. Loaded lazily by SessionMenu: Radix's DropdownMenu is
 * only needed once someone is signed in, and the shell every visitor
 * downloads has a size budget (.size-limit.json).
 */
export default function AccountMenu({
	user,
	logout,
}: {
	user: AccountUser;
	logout: () => void;
}) {
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
