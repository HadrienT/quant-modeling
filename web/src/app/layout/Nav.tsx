import { lazy, Suspense, useEffect, useState } from "react";
import { Link, useRouterState } from "@tanstack/react-router";
import { Command, Menu, Moon, Sun, X } from "lucide-react";
import { Button } from "@/shared/ui";
import { setTheme, useTheme } from "@/app/theme";
import { SessionMenu } from "@/features/auth";
import { ContextChip } from "./ContextChip";
import { NavGroups } from "./NavGroups";

const CommandPalette = lazy(() => import("@/app/command/CommandPalette"));

export function Nav() {
	const theme = useTheme();
	const [paletteOpen, setPaletteOpen] = useState(false);
	// Four spaces fit from md up (WP 28); below, they fold into a menu of
	// sections, closed on navigation and on Escape.
	const [menuOpen, setMenuOpen] = useState(false);
	const pathname = useRouterState({ select: (s) => s.location.pathname });
	useEffect(() => setMenuOpen(false), [pathname]);
	useEffect(() => {
		if (!menuOpen) return;
		const onKey = (e: KeyboardEvent) =>
			e.key === "Escape" && setMenuOpen(false);
		window.addEventListener("keydown", onKey);
		return () => window.removeEventListener("keydown", onKey);
	}, [menuOpen]);

	useEffect(() => {
		const onKey = (e: KeyboardEvent) => {
			if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
				e.preventDefault();
				setPaletteOpen(true);
			}
		};
		window.addEventListener("keydown", onKey);
		return () => window.removeEventListener("keydown", onKey);
	}, []);

	return (
		<header className="sticky top-0 z-40 border-b border-hairline bg-canvas/95 backdrop-blur-sm">
			<div className="flex h-12 min-w-0 items-center gap-1 px-4">
				<Button
					variant="ghost"
					size="icon"
					className="md:hidden"
					aria-label={menuOpen ? "Close menu" : "Open menu"}
					aria-expanded={menuOpen}
					aria-controls="mobile-nav"
					onClick={() => setMenuOpen((o) => !o)}
				>
					{menuOpen ? <X className="size-4" /> : <Menu className="size-4" />}
				</Button>

				<Link
					to="/"
					aria-label="Quant Modeling — home"
					className="mr-3 flex items-center gap-2 text-sm font-semibold text-ink"
				>
					<span className="size-4 rounded-xs bg-accent" aria-hidden="true" />
					<span className="hidden sm:inline">Quant Modeling</span>
				</Link>

				<nav
					aria-label="Spaces"
					className="hidden items-center gap-0.5 md:flex"
				>
					<NavGroups layout="bar" />
				</nav>

				<div className="ml-auto flex items-center gap-1">
					<ContextChip />
					<Button
						variant="ghost"
						size="sm"
						onClick={() => setPaletteOpen(true)}
						className="gap-1.5 text-ink-muted"
						aria-label="Open command palette"
					>
						<Command className="size-3.5" />
						<kbd className="hidden text-2xs sm:inline">⌘K</kbd>
					</Button>

					<Button
						variant="ghost"
						size="icon"
						aria-label={`Switch to ${theme === "dark" ? "light" : "dark"} theme`}
						onClick={() => setTheme(theme === "dark" ? "light" : "dark")}
					>
						{theme === "dark" ? (
							<Moon className="size-4" />
						) : (
							<Sun className="size-4" />
						)}
					</Button>

					<SessionMenu />
				</div>
			</div>

			{menuOpen && (
				<nav
					id="mobile-nav"
					aria-label="Pages"
					className="flex flex-col gap-0.5 border-t border-hairline px-4 py-2 md:hidden"
				>
					<NavGroups layout="column" />
				</nav>
			)}

			{paletteOpen && (
				<Suspense fallback={null}>
					<CommandPalette open={paletteOpen} onOpenChange={setPaletteOpen} />
				</Suspense>
			)}
		</header>
	);
}
