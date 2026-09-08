import { QueryClientProvider } from "@tanstack/react-query";
import { type ReactNode, useState } from "react";
import { makeQueryClient } from "@/shared/api";

export function QueryProvider({ children }: { children: ReactNode }) {
	const [client] = useState(makeQueryClient);
	return <QueryClientProvider client={client}>{children}</QueryClientProvider>;
}
