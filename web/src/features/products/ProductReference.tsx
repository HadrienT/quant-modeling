import { Link } from "@tanstack/react-router";
import { ArrowUpRight, ExternalLink } from "lucide-react";
import {
	CATEGORY_LABELS,
	Formula,
	InlineMath,
	PRODUCT_DOCS,
	type ProductDescriptor,
	type Reference,
} from "@/shared/products";
import { Badge } from "@/shared/ui";

/**
 * The dedicated product reference (Products page). Same content as the ⓘ
 * popover, laid out as a full page, plus a sourced bibliography — the popover
 * is for a glance, this is for reading. Content ages like a docstring;
 * `references` does not (see shared/products/docs.ts header).
 */
export function ProductReference({
	descriptor,
}: {
	descriptor: ProductDescriptor;
}) {
	const doc = descriptor.docKey ? PRODUCT_DOCS[descriptor.docKey] : undefined;
	const payoffs = doc
		? Array.isArray(doc.payoff)
			? doc.payoff
			: [doc.payoff]
		: [];

	return (
		<article className="flex max-w-3xl flex-col gap-8 pb-16">
			<header className="flex flex-col gap-3">
				<div className="flex flex-wrap items-center gap-2">
					<Badge tone="accent">{CATEGORY_LABELS[descriptor.category]}</Badge>
					{!descriptor.enabled && (
						<Badge tone="warning">Preview — engine not yet vetted</Badge>
					)}
				</div>
				<h1 className="text-2xl font-semibold text-ink">
					{doc?.title ?? descriptor.label}
				</h1>
				{doc && (
					<p className="text-sm leading-relaxed text-ink-secondary">
						<InlineMath text={doc.summary} />
					</p>
				)}
				{descriptor.enabled && (
					<Link
						to="/price"
						search={{ product: descriptor.key }}
						className="inline-flex w-fit items-center gap-1 text-sm text-accent hover:underline"
					>
						Open in the pricing workbench
						<ArrowUpRight className="size-3.5" />
					</Link>
				)}
			</header>

			{!doc ? (
				<p className="text-sm text-ink-muted">
					No reference sheet for this product yet.
				</p>
			) : (
				<>
					<Section title="Payoff">
						<div className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-5">
							{payoffs.map((p, i) => (
								<Formula key={i} tex={p} block />
							))}
						</div>
					</Section>

					<Section title="Assumptions">
						<Bullets items={doc.assumptions} />
					</Section>

					<Section title="How it's priced in this repo">
						<ul className="flex flex-col gap-3">
							{doc.pricingMethods.map((m, i) => (
								<li
									key={i}
									className="rounded-md border border-hairline bg-surface p-3 text-sm text-ink-secondary"
								>
									<span className="font-medium text-ink">{m.engine}. </span>
									<InlineMath text={m.detail} />
								</li>
							))}
						</ul>
						{descriptor.engines.length > 0 && (
							<p className="mt-2 text-2xs text-ink-muted">
								Engines exposed on the workbench:{" "}
								{descriptor.engines.map((e) => e.label).join(" · ")}
							</p>
						)}
					</Section>

					<Section title="Good to know">
						<Bullets items={doc.notes} />
					</Section>

					<Section title="References">
						<ol className="flex flex-col gap-3">
							{doc.references.map((r, i) => (
								<li key={i}>
									<ReferenceRow r={r} />
								</li>
							))}
						</ol>
						<p className="mt-3 text-2xs text-ink-muted">
							Founding work and a standard textbook chapter. Where a DOI link is
							shown its target has been checked; otherwise the citation is
							complete enough to locate the work.
						</p>
					</Section>
				</>
			)}
		</article>
	);
}

function ReferenceRow({ r }: { r: Reference }) {
	const kindTone =
		r.kind === "book" ? "neutral" : r.kind === "note" ? "warning" : "good";
	return (
		<div className="flex flex-col gap-1 rounded-md border border-hairline bg-surface p-3">
			<div className="flex items-center gap-2">
				<Badge tone={kindTone}>{r.kind}</Badge>
				<span className="text-sm font-medium text-ink">{r.label}</span>
			</div>
			<p className="text-sm text-ink-secondary">{r.cite}</p>
			{r.url && (
				<a
					href={r.url}
					target="_blank"
					rel="noreferrer"
					className="inline-flex w-fit items-center gap-1 text-2xs text-accent hover:underline"
				>
					<ExternalLink className="size-3" />
					{r.url.replace(/^https?:\/\//, "")}
				</a>
			)}
		</div>
	);
}

function Section({
	title,
	children,
}: {
	title: string;
	children: React.ReactNode;
}) {
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
				{title}
			</h2>
			<div>{children}</div>
		</section>
	);
}

function Bullets({ items }: { items: string[] }) {
	return (
		<ul className="flex list-disc flex-col gap-2 pl-5 text-sm leading-relaxed text-ink-secondary">
			{items.map((it, i) => (
				<li key={i}>
					<InlineMath text={it} />
				</li>
			))}
		</ul>
	);
}
