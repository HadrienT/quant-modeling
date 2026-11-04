import { Link } from "@tanstack/react-router";
import { Eye, EyeOff } from "lucide-react";
import { useAdminPages, useHiddenPages, useSetHiddenPages } from "@/shared/api";
import { NAV_PAGES } from "@/shared/config";
import { formatDate } from "@/shared/format";
import { useMe, useSessionActions } from "@/shared/session";
import { Button, Card, cn, toast } from "@/shared/ui";
import { EmptyState } from "@/shared/ui/states";

/**
 * Admin panel, reached from the avatar menu. Today: which pages of the
 * navigation visitors see. A switch takes effect at once, for every visitor.
 */
export default function AdminPage() {
	const me = useMe();
	const { setDialogOpen } = useSessionActions();
	const isAdmin = me.data?.is_admin === true;
	const hidden = useHiddenPages();
	const audit = useAdminPages(isAdmin);
	const save = useSetHiddenPages();

	if (me.isPending) return null;

	if (!me.data?.username) {
		return (
			<div className="mx-auto max-w-lg py-12">
				<EmptyState
					kind="sign-in"
					title="Sign in"
					description="The admin panel is for administrators."
					action={
						<Button size="sm" onClick={() => setDialogOpen(true)}>
							Sign in
						</Button>
					}
				/>
			</div>
		);
	}
	if (!isAdmin) {
		return (
			<div className="mx-auto max-w-lg py-12">
				<EmptyState
					kind="no-results"
					title="Administrators only"
					description="This account is not an administrator."
				/>
			</div>
		);
	}

	const current = new Set(hidden.data ?? []);
	function toggle(path: string, label: string) {
		const next = new Set(current);
		const hide = !next.has(path);
		if (hide) next.add(path);
		else next.delete(path);
		save.mutate([...next], {
			onSuccess: () =>
				toast.success(
					hide ? `${label} hidden from visitors` : `${label} visible again`,
				),
			onError: (e) => toast.error(`Not saved: ${e.message}`),
		});
	}

	const shown = NAV_PAGES.length - current.size;
	const last = audit.data?.updated_at;

	return (
		<div className="mx-auto flex max-w-xl flex-col gap-4">
			<div>
				<h1 className="text-lg font-semibold text-ink">Admin panel</h1>
				<p className="text-xs text-ink-secondary">
					Pages shown to visitors: {shown} of {NAV_PAGES.length}.
					{last &&
						` Last change ${formatDate(last)} by ${audit.data?.updated_by}.`}
				</p>
			</div>

			<Card className="p-0">
				<ul className="divide-y divide-hairline">
					{NAV_PAGES.map((p) => {
						const off = current.has(p.path);
						return (
							<li
								key={p.path}
								className="flex items-center justify-between gap-4 px-4 py-2.5"
							>
								<div className="flex min-w-0 items-center gap-2.5">
									{off ? (
										<EyeOff className="size-4 text-ink-muted" aria-hidden />
									) : (
										<Eye className="size-4 text-ink-secondary" aria-hidden />
									)}
									<Link
										to={p.path}
										className={cn(
											"text-sm underline-offset-2 hover:underline",
											off ? "text-ink-muted" : "text-ink",
										)}
									>
										{p.label}
									</Link>
									<code className="text-2xs text-ink-muted">{p.path}</code>
								</div>
								<Switch
									label={`Show ${p.label} to visitors`}
									checked={!off}
									disabled={hidden.isPending || save.isPending}
									onChange={() => toggle(p.path, p.label)}
								/>
							</li>
						);
					})}
				</ul>
			</Card>

			<p className="text-xs text-ink-secondary">
				A hidden page leaves the navigation and the command palette, and a
				visitor following a link to it sees “Page not available”. You keep
				seeing it, marked as hidden. Its API routes stay open: this hides a
				page, it does not lock it.
			</p>
		</div>
	);
}

function Switch({
	label,
	checked,
	disabled,
	onChange,
}: {
	label: string;
	checked: boolean;
	disabled?: boolean;
	onChange: () => void;
}) {
	return (
		<button
			type="button"
			role="switch"
			aria-checked={checked}
			aria-label={label}
			disabled={disabled}
			onClick={onChange}
			className={cn(
				"relative inline-flex h-5 w-9 shrink-0 items-center rounded-full transition-colors focus-visible:outline-2 focus-visible:outline-accent disabled:opacity-50",
				checked ? "bg-accent" : "bg-surface-raised ring-1 ring-hairline",
			)}
		>
			<span
				className={cn(
					"inline-block size-4 rounded-full bg-canvas shadow-sm transition-transform",
					checked ? "translate-x-4.5" : "translate-x-0.5",
				)}
			/>
		</button>
	);
}
