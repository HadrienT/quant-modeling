import type { ReactNode } from "react";
import { Link } from "@tanstack/react-router";
import { SITE } from "@/shared/config";
import { formatDate } from "@/shared/format";

const PAGES = [
	{ to: "/legal", label: "Legal notice" },
	{ to: "/privacy", label: "Privacy" },
	{ to: "/terms", label: "Terms & disclaimer" },
] as const;

/** The frame shared by the three legal pages: title, date, and links between them. */
export function LegalPage({
	title,
	children,
}: {
	title: string;
	children: ReactNode;
}) {
	return (
		<article className="mx-auto max-w-2xl text-sm leading-relaxed text-ink-secondary">
			<h1 className="text-lg font-semibold text-ink">{title}</h1>
			<p className="mt-1 text-xs text-ink-muted">
				Last updated {formatDate(SITE.legalUpdated)}
			</p>
			{children}
			<nav
				aria-label="Legal documents"
				className="mt-10 flex flex-wrap gap-x-4 gap-y-1 border-t border-hairline pt-4 text-xs"
			>
				{PAGES.map((p) => (
					<Link
						key={p.to}
						to={p.to}
						className="hover:text-ink [&.active]:text-ink"
					>
						{p.label}
					</Link>
				))}
			</nav>
		</article>
	);
}

export function Section({
	title,
	children,
}: {
	title: string;
	children: ReactNode;
}) {
	return (
		<section className="mt-6">
			<h2 className="text-sm font-semibold text-ink">{title}</h2>
			<div className="mt-2 space-y-2">{children}</div>
		</section>
	);
}

export function ContactLink() {
	return (
		<a className="text-accent hover:underline" href={`mailto:${SITE.contact}`}>
			{SITE.contact}
		</a>
	);
}
