import { Suspense, lazy } from "react";
import { Button } from "@/shared/ui";
import { useMe, useSessionActions } from "@/shared/session";
import { Avatar } from "./Avatar";

// Split out of the shell: only signed-in visitors download the menu (and
// Radix's DropdownMenu with it).
const AccountMenu = lazy(() => import("./AccountMenu"));

/** Session control in the nav (WP 03 §2 / WP 04): avatar → account menu. */
export function SessionMenu() {
	const me = useMe();
	const { logout, setDialogOpen } = useSessionActions();

	if (me.isLoading) return <div className="h-9 w-16" aria-hidden="true" />;

	if (me.data?.username) {
		const user = me.data;
		return (
			// While the menu's chunk loads, the same avatar, not yet clickable.
			<Suspense fallback={<Avatar me={user} className="ml-1" />}>
				<AccountMenu user={user} logout={logout} />
			</Suspense>
		);
	}

	return (
		<Button size="sm" variant="secondary" onClick={() => setDialogOpen(true)}>
			Sign in
		</Button>
	);
}
