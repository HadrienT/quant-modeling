import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { loginUser, registerUser, getMe } from "../api/client";

type AuthState = {
	token: string | null;
	username: string | null;
	loading: boolean;
	login: (username: string, password: string) => Promise<void>;
	register: (username: string, password: string) => Promise<void>;
	logout: () => void;
};

const TOKEN_KEY = "qm_auth_token";

const AuthContext = createContext<AuthState>({
	token: null,
	username: null,
	loading: true,
	login: async () => {},
	register: async () => {},
	logout: () => {},
});

// eslint-disable-next-line react-refresh/only-export-components
export function useAuth(): AuthState {
	return useContext(AuthContext);
}

export function AuthProvider({ children }: { children: ReactNode }) {
	const savedToken = localStorage.getItem(TOKEN_KEY);
	const [token, setToken] = useState<string | null>(null);
	const [username, setUsername] = useState<string | null>(null);
	const [loading, setLoading] = useState(!!savedToken); // only load if there's a token to validate
	const didInit = useRef(false);

	// On mount: validate persisted token
	useEffect(() => {
		if (didInit.current || !savedToken) return;
		didInit.current = true;
		getMe(savedToken)
			.then((u) => {
				setToken(savedToken);
				setUsername(u.username);
			})
			.catch(() => {
				localStorage.removeItem(TOKEN_KEY);
			})
			.finally(() => setLoading(false));
	}, [savedToken]);

	const login = useCallback(async (user: string, pass: string) => {
		const res = await loginUser(user, pass);
		setToken(res.token);
		setUsername(res.username);
		localStorage.setItem(TOKEN_KEY, res.token);
	}, []);

	const register = useCallback(async (user: string, pass: string) => {
		const res = await registerUser(user, pass);
		setToken(res.token);
		setUsername(res.username);
		localStorage.setItem(TOKEN_KEY, res.token);
	}, []);

	const logout = useCallback(() => {
		setToken(null);
		setUsername(null);
		localStorage.removeItem(TOKEN_KEY);
	}, []);

	return (
		<AuthContext.Provider value={{ token, username, loading, login, register, logout }}>
			{children}
		</AuthContext.Provider>
	);
}
