import { type FormEvent, useEffect, useState } from "react";
import { LogOut, UserRound } from "lucide-react";
import { useAuth } from "@/auth/AuthContext";
import { setAuthToken } from "@/shared/api";
import {
	Button,
	Dialog,
	DialogContent,
	DialogDescription,
	DialogHeader,
	DialogTitle,
	Input,
	Label,
} from "@/shared/ui";

/**
 * Session control in the nav (WP 03 §2). The accessible auth dialog replaces the
 * hand-rolled modal from App.tsx (WP 01 §7). WP 04 adds expiry handling, local
 * portfolio migration and the repository boundary.
 */
export function SessionMenu() {
	const { username, token, loading, login, register, logout } = useAuth();

	// Bridge the legacy token into the new typed client until WP 04.
	useEffect(() => {
		setAuthToken(token);
	}, [token]);

	const [open, setOpen] = useState(false);

	if (loading) return <div className="h-9 w-16" aria-hidden="true" />;

	if (username) {
		return (
			<div className="flex items-center gap-1.5 pl-1">
				<span className="flex items-center gap-1 text-xs text-ink-secondary">
					<UserRound className="size-3.5" />
					{username}
				</span>
				<Button
					variant="ghost"
					size="icon"
					aria-label="Log out"
					onClick={logout}
				>
					<LogOut className="size-4" />
				</Button>
			</div>
		);
	}

	return (
		<>
			<Button size="sm" variant="secondary" onClick={() => setOpen(true)}>
				Sign in
			</Button>
			<AuthDialog
				open={open}
				onOpenChange={setOpen}
				onLogin={login}
				onRegister={register}
			/>
		</>
	);
}

function AuthDialog({
	open,
	onOpenChange,
	onLogin,
	onRegister,
}: {
	open: boolean;
	onOpenChange: (v: boolean) => void;
	onLogin: (u: string, p: string) => Promise<void>;
	onRegister: (u: string, p: string) => Promise<void>;
}) {
	const [mode, setMode] = useState<"login" | "register">("login");
	const [error, setError] = useState<string | null>(null);
	const [busy, setBusy] = useState(false);

	async function onSubmit(e: FormEvent<HTMLFormElement>) {
		e.preventDefault();
		const form = new FormData(e.currentTarget);
		const user = String(form.get("username") ?? "").trim();
		const pass = String(form.get("password") ?? "");
		if (!user || !pass) return;
		setBusy(true);
		setError(null);
		try {
			await (mode === "login" ? onLogin : onRegister)(user, pass);
			onOpenChange(false);
		} catch (err) {
			setError(err instanceof Error ? err.message : "Authentication failed.");
		} finally {
			setBusy(false);
		}
	}

	return (
		<Dialog open={open} onOpenChange={onOpenChange}>
			<DialogContent>
				<DialogHeader>
					<DialogTitle>
						{mode === "login" ? "Sign in" : "Create an account"}
					</DialogTitle>
					<DialogDescription>
						An account only stores server-side portfolios. Pricing, market data,
						strategies and backtests work without one.
					</DialogDescription>
				</DialogHeader>

				<form onSubmit={onSubmit} className="flex flex-col gap-3">
					<div className="flex flex-col gap-1">
						<Label htmlFor="auth-username">Username</Label>
						<Input id="auth-username" name="username" autoComplete="username" />
					</div>
					<div className="flex flex-col gap-1">
						<Label htmlFor="auth-password">Password</Label>
						<Input
							id="auth-password"
							name="password"
							type="password"
							autoComplete={
								mode === "login" ? "current-password" : "new-password"
							}
						/>
					</div>

					{error && (
						<p role="alert" className="text-xs text-critical">
							{error}
						</p>
					)}

					<Button type="submit" disabled={busy}>
						{busy ? "…" : mode === "login" ? "Sign in" : "Register"}
					</Button>
					<button
						type="button"
						className="text-xs text-ink-secondary underline-offset-2 hover:underline"
						onClick={() => {
							setMode(mode === "login" ? "register" : "login");
							setError(null);
						}}
					>
						{mode === "login"
							? "No account? Register"
							: "Already have an account? Sign in"}
					</button>
				</form>
			</DialogContent>
		</Dialog>
	);
}
