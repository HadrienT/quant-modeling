import { type ReactNode } from "react";
import { Toaster, TooltipProvider } from "@/shared/ui";
import { SessionProvider } from "@/shared/session";
import { AuthDialog } from "@/features/auth";
import { QueryProvider } from "./QueryProvider";

export function AppProviders({ children }: { children: ReactNode }) {
	return (
		<QueryProvider>
			<SessionProvider>
				<TooltipProvider delayDuration={300}>{children}</TooltipProvider>
				<AuthDialog />
				<Toaster />
			</SessionProvider>
		</QueryProvider>
	);
}
