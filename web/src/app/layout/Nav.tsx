import { lazy, Suspense, useEffect, useState } from "react";
import { Link } from "@tanstack/react-router";
import { Command, Moon, Sun } from "lucide-react";
import { Button, cn } from "@/shared/ui";
import { setTheme, useTheme } from "@/app/theme";
import { ROUTES } from "@/app/router";
import { SessionMenu } from "@/features/auth";

const CommandPalette = lazy(() => import("@/app/command/CommandPalette"));

export function Nav() {
	const theme = useTheme();
	const [paletteOpen, setPaletteOpen] = useState(false);

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
			<div className="flex h-12 items-center gap-1 px-4">
				<Link
					to="/visualize"
					className="mr-3 flex items-center gap-2 text-sm font-semibold text-ink"
				>
					<span className="size-4 rounded-xs bg-accent" aria-hidden="true" />
					Quant Modeling
				</Link>

				<nav className="flex items-center gap-0.5">
					{ROUTES.map((r) => (
						<Link
							key={r.path}
							to={r.path}
							preload="intent"
							className={cn(
								"rounded-sm px-2.5 py-1 text-sm text-ink-secondary transition-colors hover:bg-surface-raised hover:text-ink",
							)}
							activeProps={{ className: "bg-surface-raised !text-ink" }}
						>
							{r.label}
						</Link>
					))}
				</nav>

				<div className="ml-auto flex items-center gap-1">
					<Button
						variant="ghost"
						size="sm"
						onClick={() => setPaletteOpen(true)}
						className="gap-1.5 text-ink-muted"
						aria-label="Open command palette"
					>
						<Command className="size-3.5" />
						<kbd className="text-2xs">⌘K</kbd>
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

			{paletteOpen && (
				<Suspense fallback={null}>
					<CommandPalette open={paletteOpen} onOpenChange={setPaletteOpen} />
				</Suspense>
			)}
		</header>
	);
}
