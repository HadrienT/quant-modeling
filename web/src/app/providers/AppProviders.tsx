import { type ReactNode } from "react";
import { Toaster, TooltipProvider } from "@/shared/ui";
import { AuthProvider } from "@/auth/AuthContext";
import { QueryProvider } from "./QueryProvider";

/**
 * Provider composition. AuthProvider is still the legacy context (WP 04 replaces
 * it); it is kept here so the not-yet-migrated Portfolio page keeps working.
 */
export function AppProviders({ children }: { children: ReactNode }) {
	return (
		<QueryProvider>
			<AuthProvider>
				<TooltipProvider delayDuration={300}>{children}</TooltipProvider>
				<Toaster />
			</AuthProvider>
		</QueryProvider>
	);
}
