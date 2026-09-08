import { useState } from "react";
import { useForm } from "react-hook-form";
import { zodResolver } from "@hookform/resolvers/zod";
import { z } from "zod";
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
import { useSessionActions } from "@/shared/session";

const schema = z.object({
	username: z.string().min(2, "At least 2 characters"),
	password: z.string().min(8, "At least 8 characters"),
});
type Values = z.infer<typeof schema>;

/** Accessible auth dialog (WP 04 §1): Dialog primitive, RHF + zod, errors via role=alert. */
export function AuthDialog() {
	const { login, register, dialogOpen, setDialogOpen, expired } =
		useSessionActions();
	const [mode, setMode] = useState<"login" | "register">("login");
	const [serverError, setServerError] = useState<string | null>(null);

	const {
		register: field,
		handleSubmit,
		formState: { errors, isSubmitting },
		reset,
	} = useForm<Values>({ resolver: zodResolver(schema) });

	async function onSubmit(v: Values) {
		setServerError(null);
		try {
			await (mode === "login" ? login : register)(v.username, v.password);
			setDialogOpen(false);
			reset();
		} catch (e) {
			setServerError(e instanceof Error ? e.message : "Authentication failed.");
		}
	}

	return (
		<Dialog open={dialogOpen} onOpenChange={setDialogOpen}>
			<DialogContent>
				<DialogHeader>
					<DialogTitle>
						{mode === "login" ? "Sign in" : "Create an account"}
					</DialogTitle>
					<DialogDescription>
						{expired
							? "Your session expired — sign in again to continue. Your work on this page is kept."
							: "An account only stores server-side portfolios. Everything else works without one."}
					</DialogDescription>
				</DialogHeader>

				<form onSubmit={handleSubmit(onSubmit)} className="flex flex-col gap-3">
					<div className="flex flex-col gap-1">
						<Label htmlFor="auth-username">Username</Label>
						<Input
							id="auth-username"
							autoComplete="username"
							{...field("username")}
						/>
						{errors.username && (
							<span role="alert" className="text-2xs text-critical">
								{errors.username.message}
							</span>
						)}
					</div>
					<div className="flex flex-col gap-1">
						<Label htmlFor="auth-password">Password</Label>
						<Input
							id="auth-password"
							type="password"
							autoComplete={
								mode === "login" ? "current-password" : "new-password"
							}
							{...field("password")}
						/>
						{errors.password && (
							<span role="alert" className="text-2xs text-critical">
								{errors.password.message}
							</span>
						)}
					</div>

					{serverError && (
						<p role="alert" className="text-xs text-critical">
							{serverError}
						</p>
					)}

					<Button type="submit" disabled={isSubmitting}>
						{isSubmitting ? "…" : mode === "login" ? "Sign in" : "Register"}
					</Button>
					<button
						type="button"
						className="text-xs text-ink-secondary underline-offset-2 hover:underline"
						onClick={() => {
							setMode(mode === "login" ? "register" : "login");
							setServerError(null);
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
