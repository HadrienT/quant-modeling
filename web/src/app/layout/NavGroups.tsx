import { useEffect, useRef, useState } from "react";
import { Link, useRouterState } from "@tanstack/react-router";
import { ChevronDown } from "lucide-react";
import { cn } from "@/shared/ui";
import {
	useNavPages,
	type VisibleGroup,
	type VisiblePage,
} from "@/shared/session";

/**
 * The navigation as four spaces (WP 28): Market, Price, Risk, Learn. On wide
 * screens each space is a menu of its pages; in the folded menu the spaces
 * are sections, every page in sight. A page hidden from visitors shows to an
 * administrator, faded and struck through.
 */
export function NavGroups({ layout }: { layout: "bar" | "column" }) {
	const { groups } = useNavPages();
	if (layout === "column")
		return groups.map((g) => (
			<section key={g.key} className="flex flex-col gap-0.5 py-1">
				<h2 className="px-2.5 text-2xs font-semibold tracking-wide text-ink-muted uppercase">
					{g.label}
				</h2>
				{g.pages.map((p) => (
					<PageLink key={p.path} page={p} />
				))}
			</section>
		));
	return groups.map((g) => <GroupMenu key={g.key} group={g} />);
}

/**
 * A disclosure, not a menu widget: a button that shows a panel of plain
 * links, which is what a site navigation is. It closes on Escape, on a click
 * or a focus that leaves it, and on navigation.
 */
function GroupMenu({ group }: { group: VisibleGroup }) {
	const [open, setOpen] = useState(false);
	const root = useRef<HTMLDivElement>(null);
	const pathname = useRouterState({ select: (s) => s.location.pathname });
	useEffect(() => setOpen(false), [pathname]);
	useEffect(() => {
		if (!open) return;
		const onDown = (e: PointerEvent) => {
			if (!root.current?.contains(e.target as Node)) setOpen(false);
		};
		const onKey = (e: KeyboardEvent) => e.key === "Escape" && setOpen(false);
		document.addEventListener("pointerdown", onDown);
		document.addEventListener("keydown", onKey);
		return () => {
			document.removeEventListener("pointerdown", onDown);
			document.removeEventListener("keydown", onKey);
		};
	}, [open]);
	const active = group.pages.some((p) => p.path === pathname);
	const panelId = `nav-space-${group.key}`;
	return (
		<div
			ref={root}
			className="relative"
			onBlur={(e) => {
				if (!e.currentTarget.contains(e.relatedTarget)) setOpen(false);
			}}
		>
			<button
				type="button"
				aria-expanded={open}
				aria-controls={panelId}
				onClick={() => setOpen((o) => !o)}
				className={cn(
					"flex items-center gap-1 rounded-sm px-2.5 py-1 text-sm text-ink-secondary transition-colors hover:bg-surface-raised hover:text-ink",
					(active || open) && "bg-surface-raised text-ink",
				)}
			>
				{group.label}
				<ChevronDown
					className={cn(
						"size-3.5 text-ink-muted transition-transform",
						open && "rotate-180",
					)}
					aria-hidden="true"
				/>
			</button>
			{open && (
				<div
					id={panelId}
					className="absolute top-full left-0 z-50 mt-1.5 flex w-80 flex-col gap-0.5 rounded-md border border-hairline bg-surface p-1.5 shadow-lg"
				>
					<p className="px-2.5 pt-1 pb-1.5 text-2xs tracking-wide text-ink-muted uppercase">
						{group.question}
					</p>
					{group.pages.map((p) => (
						<PageLink key={p.path} page={p} withHint />
					))}
				</div>
			)}
		</div>
	);
}

function PageLink({
	page,
	withHint = false,
}: {
	page: VisiblePage;
	withHint?: boolean;
}) {
	return (
		<Link
			to={page.path}
			preload="intent"
			className="flex flex-col rounded-sm px-2.5 py-1.5 text-sm text-ink-secondary transition-colors hover:bg-surface-raised hover:text-ink"
			activeProps={{ className: "bg-surface-raised !text-ink" }}
			title={page.hidden ? "Hidden from visitors" : undefined}
		>
			{page.hidden ? (
				<span className="line-through opacity-60">
					<span className="sr-only">hidden from visitors: </span>
					{page.label}
				</span>
			) : (
				<span className={cn(withHint && "font-medium text-ink")}>
					{page.label}
				</span>
			)}
			{withHint && <span className="text-xs text-ink-muted">{page.hint}</span>}
		</Link>
	);
}
