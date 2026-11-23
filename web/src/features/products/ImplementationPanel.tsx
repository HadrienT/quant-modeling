import { ChevronRight, ExternalLink } from "lucide-react";
import {
	CITE,
	headlineOf,
	implementationOf,
	InlineMath,
	type Implementation,
	type ScriptedProduct,
} from "@/shared/products";
import { Badge } from "@/shared/ui";

/**
 * "How it is implemented": a closed panel on a product's sheet listing what
 * its price goes through on the site (model, calibration, scheme, random
 * numbers, variance reduction, multilevel or not, early exercise, greeks,
 * device) and what a price to 0.1 % measured, every method cited as [n]
 * with the references in an appendix below. Closed by default: it is long.
 */
export function ImplementationPanel({ product }: { product: ScriptedProduct }) {
	const impl = implementationOf(product);
	if (!impl) return null;
	const chips = headlineOf(product.slug);
	return (
		<details className="group rounded-md border border-hairline bg-surface">
			<summary className="flex cursor-pointer list-none flex-wrap items-center gap-2 p-4 select-none [&::-webkit-details-marker]:hidden">
				<ChevronRight
					className="size-4 shrink-0 text-ink-muted transition-transform group-open:rotate-90"
					aria-hidden
				/>
				<h2 className="text-sm font-semibold text-ink">
					How it is implemented
				</h2>
				{chips.map((c) => (
					<Badge key={c} tone="neutral">
						{c}
					</Badge>
				))}
			</summary>
			<div className="flex flex-col gap-6 border-t border-hairline p-4">
				{"notice" in impl ? (
					<p className="text-sm text-ink-secondary">{impl.notice}</p>
				) : (
					<Body impl={impl} />
				)}
			</div>
		</details>
	);
}

function Body({ impl }: { impl: Implementation }) {
	const number = new Map(impl.refs.map((r, i) => [r.key, i + 1]));
	return (
		<>
			{impl.sections.map((s) => (
				<section key={s.title} className="flex flex-col gap-2">
					<h3 className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
						{s.title}
					</h3>
					<dl className="grid grid-cols-1 gap-x-4 gap-y-2 text-sm sm:grid-cols-[10rem_1fr]">
						{s.lines.map((l) => (
							<div key={l.label} className="contents">
								<dt className="font-medium text-ink">{l.label}</dt>
								<dd className="leading-relaxed text-ink-secondary">
									<Cited text={l.text} number={number} />
								</dd>
							</div>
						))}
					</dl>
				</section>
			))}
			<p className="text-2xs text-ink-muted">
				Measured {impl.measuredAt.slice(0, 10)} on {impl.machine}, by
				scripts/bench_products.py: the page's default market and terms, model
				"auto", the price only. The method lines are read off the engine's own
				diagnostics for that run.
			</p>
			<section className="flex flex-col gap-2">
				<h3 className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
					Appendix: references
				</h3>
				<ol className="flex flex-col gap-2 text-sm">
					{impl.refs.map(({ key, ref }, i) => (
						<li
							key={key}
							id={`impl-ref-${i + 1}`}
							className="flex scroll-mt-20 gap-2 target:bg-accent/10"
						>
							<span className="w-8 shrink-0 text-right font-mono text-ink-muted tabular-nums">
								[{i + 1}]
							</span>
							<span className="text-ink-secondary">
								<span className="font-medium text-ink">{ref.label}.</span>{" "}
								{ref.cite}
								{"url" in ref && ref.url && (
									<a
										href={ref.url}
										target="_blank"
										rel="noreferrer"
										className="ml-1 inline-flex items-center gap-0.5 text-2xs text-accent hover:underline"
									>
										<ExternalLink className="size-3" />
										doi
									</a>
								)}
							</span>
						</li>
					))}
				</ol>
			</section>
		</>
	);
}

/** Prose with `$...$` math and `[@a; @b]` citations shown as [1, 2]. */
function Cited({
	text,
	number,
}: {
	text: string;
	number: Map<string, number>;
}) {
	const parts: React.ReactNode[] = [];
	let last = 0;
	for (const m of text.matchAll(CITE)) {
		parts.push(<InlineMath key={last} text={text.slice(last, m.index)} />);
		const ns = (m[1] ?? "")
			.split(/;\s*@?/)
			.map((k) => number.get(k.replace("@", "").trim()))
			.filter((n): n is number => n != null);
		parts.push(
			<span key={`c${m.index}`} className="whitespace-nowrap">
				[
				{ns.map((n, i) => (
					<span key={n}>
						{i > 0 && ", "}
						<a
							href={`#impl-ref-${n}`}
							onClick={(e) => {
								e.preventDefault();
								document
									.getElementById(`impl-ref-${n}`)
									?.scrollIntoView({ behavior: "smooth", block: "center" });
							}}
							className="text-accent hover:underline"
						>
							{n}
						</a>
					</span>
				))}
				]
			</span>,
		);
		last = m.index + m[0].length;
	}
	parts.push(<InlineMath key="end" text={text.slice(last)} />);
	return <>{parts}</>;
}
