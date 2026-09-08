import { useEffect } from "react";
import { Command } from "cmdk";
import * as DialogPrimitive from "@radix-ui/react-dialog";
import { useNavigate } from "@tanstack/react-router";
import { ArrowRight, Link2, Moon, Sun } from "lucide-react";
import { ROUTES } from "@/app/router";
import { toggleTheme, useTheme } from "@/app/theme";
import { copyText, toast } from "@/shared/ui";

/**
 * ⌘K palette (WP 03 §3) — the way to move fast in a seven-screen tool.
 * Lazy-loaded (it carries a search index). Ticker / product entries are
 * registered by their features later.
 */
export default function CommandPalette({
	open,
	onOpenChange,
}: {
	open: boolean;
	onOpenChange: (v: boolean) => void;
}) {
	const navigate = useNavigate();
	const theme = useTheme();

	useEffect(() => {
		const onKey = (e: KeyboardEvent) => {
			if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
				e.preventDefault();
				onOpenChange(!open);
			}
		};
		document.addEventListener("keydown", onKey);
		return () => document.removeEventListener("keydown", onKey);
	}, [open, onOpenChange]);

	function run(fn: () => void) {
		onOpenChange(false);
		fn();
	}

	return (
		<DialogPrimitive.Root open={open} onOpenChange={onOpenChange}>
			<DialogPrimitive.Portal>
				<DialogPrimitive.Overlay className="fixed inset-0 z-50 bg-[color-mix(in_srgb,var(--color-canvas)_72%,transparent)]" />
				<DialogPrimitive.Content
					aria-label="Command palette"
					className="fixed top-[12vh] left-1/2 z-50 w-full max-w-lg -translate-x-1/2 overflow-hidden rounded-lg border border-hairline bg-surface shadow-2xl focus:outline-none"
				>
					<Command loop>
						<Command.Input
							placeholder="Go to a page, toggle the theme, copy the URL…"
							className="w-full border-b border-hairline bg-transparent px-4 py-3 text-sm text-ink outline-none placeholder:text-ink-muted"
						/>
						<Command.List className="max-h-80 overflow-y-auto p-1.5">
							<Command.Empty className="px-3 py-6 text-center text-sm text-ink-muted">
								Nothing matches.
							</Command.Empty>

							<Command.Group
								heading="Navigate"
								className="px-1.5 py-1 text-2xs tracking-wide text-ink-muted uppercase"
							>
								{ROUTES.map((r) => (
									<Item
										key={r.path}
										onSelect={() => run(() => navigate({ to: r.path }))}
									>
										<ArrowRight className="size-3.5" />
										{r.label}
									</Item>
								))}
							</Command.Group>

							<Command.Group
								heading="Actions"
								className="px-1.5 py-1 text-2xs tracking-wide text-ink-muted uppercase"
							>
								<Item onSelect={() => run(toggleTheme)}>
									{theme === "dark" ? (
										<Sun className="size-3.5" />
									) : (
										<Moon className="size-3.5" />
									)}
									Switch to {theme === "dark" ? "light" : "dark"} theme
								</Item>
								<Item
									onSelect={() =>
										run(() => {
											void copyText(window.location.href).then((ok) =>
												ok
													? toast.success("URL copied")
													: toast.error(
															"Couldn't reach the clipboard — copy the URL from the address bar",
														),
											);
										})
									}
								>
									<Link2 className="size-3.5" />
									Copy link to this view
								</Item>
							</Command.Group>
						</Command.List>
					</Command>
				</DialogPrimitive.Content>
			</DialogPrimitive.Portal>
		</DialogPrimitive.Root>
	);
}

function Item({
	children,
	onSelect,
}: {
	children: React.ReactNode;
	onSelect: () => void;
}) {
	return (
		<Command.Item
			onSelect={onSelect}
			className="flex cursor-pointer items-center gap-2 rounded-sm px-2.5 py-1.5 text-sm text-ink-secondary data-[selected=true]:bg-surface-raised data-[selected=true]:text-ink"
		>
			{children}
		</Command.Item>
	);
}
