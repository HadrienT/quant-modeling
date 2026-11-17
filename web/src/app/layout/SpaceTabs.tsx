import { Link, useRouterState } from "@tanstack/react-router";
import { useNavPages } from "@/shared/session";

/**
 * The pages of the current space, under the bar (WP 28, lot U5): where the
 * visitor is, and its sibling pages one click away, without opening a menu.
 * Nothing shows outside a space or in a space with a single page left.
 */
export function SpaceTabs() {
	const { groups } = useNavPages();
	const pathname = useRouterState({ select: (s) => s.location.pathname });
	const group = groups.find((g) => g.pages.some((p) => p.path === pathname));
	if (!group || group.pages.length < 2) return null;
	return (
		<nav
			aria-label={`${group.label} pages`}
			className="flex items-center gap-1 overflow-x-auto border-b border-hairline px-4"
		>
			<span className="mr-2 text-2xs font-semibold tracking-wide text-ink-muted uppercase">
				{group.label}
			</span>
			{group.pages.map((p) => (
				<Link
					key={p.path}
					to={p.path}
					preload="intent"
					title={p.hidden ? "Hidden from visitors" : p.hint}
					className="-mb-px border-b-2 border-transparent px-2.5 py-2 text-xs whitespace-nowrap text-ink-secondary transition-colors hover:text-ink"
					activeProps={{
						className: "!border-accent !text-ink",
						"aria-current": "page",
					}}
				>
					{p.hidden ? (
						<span className="line-through opacity-60">{p.label}</span>
					) : (
						p.label
					)}
				</Link>
			))}
		</nav>
	);
}
