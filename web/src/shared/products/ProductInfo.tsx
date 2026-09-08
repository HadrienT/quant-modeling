import { Info } from "lucide-react";
import { Popover, PopoverContent, PopoverTrigger } from "@/shared/ui";
import { Formula } from "./Formula";
import { PRODUCT_DOCS, type ProductDoc } from "./docs";

/**
 * ⓘ doc card — blueprint WP 99 migration. Now a Radix Popover (collision
 * detection, stays anchored while scrolling), and the trigger is a real
 * <button> OUTSIDE any <label> (nested interactive content is invalid HTML).
 * No icon renders when the key has no entry — partial rollout stays silent.
 */
export function ProductInfo({ docKey }: { docKey?: string }) {
	const doc = docKey ? PRODUCT_DOCS[docKey] : undefined;
	if (!doc) return null;
	return (
		<Popover>
			<PopoverTrigger asChild>
				<button
					type="button"
					aria-label={`About ${doc.title}`}
					className="text-ink-muted transition-colors hover:text-accent"
				>
					<Info className="size-3.5" />
				</button>
			</PopoverTrigger>
			<PopoverContent align="start" className="w-96 max-w-[92vw]">
				<DocBody doc={doc} />
			</PopoverContent>
		</Popover>
	);
}

function DocBody({ doc }: { doc: ProductDoc }) {
	const payoffs = Array.isArray(doc.payoff) ? doc.payoff : [doc.payoff];
	return (
		<div className="flex max-h-[70vh] flex-col gap-3 overflow-y-auto pr-1">
			<div>
				<h3 className="text-sm font-semibold text-ink">{doc.title}</h3>
				<p className="mt-1 text-xs text-ink-secondary">
					<Inline text={doc.summary} />
				</p>
			</div>

			<Section title="Payoff">
				{payoffs.map((p, i) => (
					<Formula key={i} tex={p} block />
				))}
			</Section>

			<Section title="Assumptions">
				<Bullets items={doc.assumptions} />
			</Section>

			<Section title="How it's priced here">
				<ul className="flex flex-col gap-1.5">
					{doc.pricingMethods.map((m, i) => (
						<li key={i} className="text-xs text-ink-secondary">
							<span className="font-medium text-ink">{m.engine}. </span>
							<Inline text={m.detail} />
						</li>
					))}
				</ul>
			</Section>

			<Section title="Good to know">
				<Bullets items={doc.notes} />
			</Section>
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
		<div>
			<h4 className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
				{title}
			</h4>
			<div className="mt-1">{children}</div>
		</div>
	);
}

function Bullets({ items }: { items: string[] }) {
	return (
		<ul className="flex list-disc flex-col gap-1 pl-4 text-xs text-ink-secondary">
			{items.map((it, i) => (
				<li key={i}>
					<Inline text={it} />
				</li>
			))}
		</ul>
	);
}

/** Render inline `$...$` math inside a text string. */
function Inline({ text }: { text: string }) {
	const parts = text.split(/(\$[^$]+\$)/g);
	return (
		<>
			{parts.map((p, i) =>
				p.startsWith("$") && p.endsWith("$") ? (
					<Formula key={i} tex={p.slice(1, -1)} />
				) : (
					<span key={i}>{p}</span>
				),
			)}
		</>
	);
}
