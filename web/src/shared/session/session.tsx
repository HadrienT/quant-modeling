import {
	type ReactNode,
	createContext,
	useCallback,
	useContext,
	useEffect,
	useState,
} from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { ApiError, api, setAuthToken } from "@/shared/api";

/**
 * Session = server state (WP 04 §2). useMe() is a TanStack Query; the context
 * only exposes actions. A single invalidation after login / logout puts the
 * whole app right.
 *
 * Token storage: localStorage. This is exfiltrable by XSS — an accepted risk
 * until the API moves to an httpOnly cookie (WP 04 server task / WP 14 CSP).
 */

const TOKEN_KEY = "qm_auth_token";

function readToken(): string | null {
	try {
		return localStorage.getItem(TOKEN_KEY);
	} catch {
		return null;
	}
}
function persistToken(t: string | null) {
	try {
		if (t) localStorage.setItem(TOKEN_KEY, t);
		else localStorage.removeItem(TOKEN_KEY);
	} catch {
		/* private mode */
	}
}

type Ctx = {
	login: (u: string, p: string) => Promise<void>;
	register: (u: string, p: string) => Promise<void>;
	logout: () => void;
	/** open the auth dialog, e.g. from a 401 interceptor */
	promptSignIn: () => void;
	dialogOpen: boolean;
	setDialogOpen: (v: boolean) => void;
	expired: boolean;
};

const SessionContext = createContext<Ctx | null>(null);

export function useSessionActions(): Ctx {
	const ctx = useContext(SessionContext);
	if (!ctx) throw new Error("useSessionActions outside <SessionProvider>");
	return ctx;
}

/** Current user (or null). Server state — do not mirror into a store. */
export function useMe() {
	return useQuery({
		queryKey: ["session"],
		queryFn: async () => {
			if (!readToken()) return null;
			const { data, error } = await api.GET("/api/auth/me", {});
			if (error) throw ApiError.from(error);
			return data ?? null;
		},
		staleTime: 5 * 60_000,
		retry: false,
	});
}

export function SessionProvider({ children }: { children: ReactNode }) {
	const qc = useQueryClient();
	const [dialogOpen, setDialogOpen] = useState(false);
	const [expired, setExpired] = useState(false);

	useEffect(() => {
		setAuthToken(readToken());
	}, []);

	const applyToken = useCallback(
		async (token: string) => {
			persistToken(token);
			setAuthToken(token);
			setExpired(false);
			await qc.invalidateQueries();
		},
		[qc],
	);

	const login = useCallback(
		async (username: string, password: string) => {
			const { data, error } = await api.POST("/api/auth/login", {
				body: { username, password },
			});
			if (error) throw ApiError.from(error);
			await applyToken(data!.token);
		},
		[applyToken],
	);

	const register = useCallback(
		async (username: string, password: string) => {
			const { data, error } = await api.POST("/api/auth/register", {
				body: { username, password },
			});
			if (error) throw ApiError.from(error);
			await applyToken(data!.token);
		},
		[applyToken],
	);

	const logout = useCallback(() => {
		persistToken(null);
		setAuthToken(null);
		void qc.invalidateQueries();
	}, [qc]);

	const promptSignIn = useCallback(() => {
		if (readToken()) {
			// had a token, must have expired
			persistToken(null);
			setAuthToken(null);
			setExpired(true);
			void qc.invalidateQueries({ queryKey: ["session"] });
		}
		setDialogOpen(true);
	}, [qc]);

	// 401 interceptor: any user-scoped request that 401s ends the session but
	// keeps the current page (the form is not thrown away).
	useEffect(() => {
		const mw = {
			onResponse({ response }: { response: Response }) {
				if (
					response.status === 401 &&
					response.url.includes("/api/portfolios")
				) {
					promptSignIn();
				}
				return response;
			},
		};
		api.use(mw);
		return () => api.eject(mw);
	}, [promptSignIn]);

	return (
		<SessionContext.Provider
			value={{
				login,
				register,
				logout,
				promptSignIn,
				dialogOpen,
				setDialogOpen,
				expired,
			}}
		>
			{children}
		</SessionContext.Provider>
	);
}

export const __TOKEN_KEY = TOKEN_KEY;
