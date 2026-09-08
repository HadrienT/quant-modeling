import { Suspense, lazy, useEffect, useState } from "react";
import { useSessionActions } from "@/shared/session";

const AuthDialog = lazy(() =>
	import("./AuthDialog").then((m) => ({ default: m.AuthDialog })),
);

/**
 * Mounts the auth dialog (and its react-hook-form + zod payload) only after it
 * has been opened once — keeps that weight out of the initial chunk (WP 13).
 */
export function AuthDialogGate() {
	const { dialogOpen } = useSessionActions();
	const [everOpened, setEverOpened] = useState(false);
	useEffect(() => {
		if (dialogOpen) setEverOpened(true);
	}, [dialogOpen]);

	if (!everOpened) return null;
	return (
		<Suspense fallback={null}>
			<AuthDialog />
		</Suspense>
	);
}
