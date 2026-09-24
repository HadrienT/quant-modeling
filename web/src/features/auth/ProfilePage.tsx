import type { ReactNode } from "react";
import { Link } from "@tanstack/react-router";
import { LogOut } from "lucide-react";
import { usePortfolios } from "@/shared/api";
import { formatDate } from "@/shared/format";
import { useMe, useSessionActions } from "@/shared/session";
import { Button, Card } from "@/shared/ui";
import { displayName } from "./account";
import { Avatar } from "./Avatar";

function Row({ label, children }: { label: string; children: ReactNode }) {
	return (
		<div className="flex items-baseline justify-between gap-4 py-2">
			<dt className="text-xs text-ink-secondary">{label}</dt>
			<dd className="text-sm text-ink">{children}</dd>
		</div>
	);
}

/** Account page, reached from the avatar menu in the nav. */
export default function ProfilePage() {
	const me = useMe();
	const { logout, setDialogOpen } = useSessionActions();
	const user = me.data;
	const portfolios = usePortfolios();

	if (me.isLoading) return null;

	if (!user?.username) {
		return (
			<div className="mx-auto flex max-w-md flex-col items-center gap-3 py-16 text-sm text-ink-secondary">
				<p>Sign in to see your profile.</p>
				<Button size="sm" onClick={() => setDialogOpen(true)}>
					Sign in
				</Button>
			</div>
		);
	}

	const google = user.provider === "google";

	return (
		<div className="mx-auto flex max-w-xl flex-col gap-4">
			<div className="flex items-center gap-3">
				<Avatar me={user} className="size-12 text-lg" />
				<div className="min-w-0">
					<h1 className="truncate text-lg font-semibold text-ink">
						{displayName(user)}
					</h1>
					<p className="text-xs text-ink-secondary">
						{google ? "Google account" : "Password account"}
					</p>
				</div>
			</div>

			<Card>
				<dl className="divide-y divide-hairline">
					{user.email && <Row label="Email">{user.email}</Row>}
					{!google && <Row label="Username">{user.username}</Row>}
					<Row label="Sign-in method">{google ? "Google" : "Password"}</Row>
					<Row label="Member since">{formatDate(user.created_at)}</Row>
					<Row label="Server-side portfolios">
						<Link
							to="/portfolio"
							className="underline-offset-2 hover:underline"
						>
							{portfolios.data ? portfolios.data.length : "—"}
						</Link>
					</Row>
				</dl>
			</Card>

			<p className="text-xs text-ink-secondary">
				An account only stores your portfolios on the server. Pricing, market
				data and the rest of the site work without one.
			</p>

			<div>
				<Button variant="secondary" size="sm" onClick={logout}>
					<LogOut className="size-4" />
					Sign out
				</Button>
			</div>
		</div>
	);
}
