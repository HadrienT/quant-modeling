import { useEffect, useRef, useState, type ReactNode } from "react";
import { createPortal } from "react-dom";
import type { ProductDoc } from "../pages/pricing/productDocs";
import Formula from "./Formula";

type Props = {
	/** Looked up from PRODUCT_DOCS by the caller; renders nothing when absent. */
	doc?: ProductDoc;
};

/** Splits `text` on `$...$` segments, rendering the math parts with KaTeX. */
function renderMathText(text: string, keyPrefix: string): ReactNode {
	return text
		.split(/\$([^$]+)\$/g)
		.map((part, i) => (i % 2 === 1 ? <Formula key={`${keyPrefix}-${i}`} tex={part} /> : <span key={`${keyPrefix}-${i}`}>{part}</span>));
}

const PANEL_WIDTH = 380;

/**
 * A small "ⓘ" trigger that opens a floating panel with the pedagogical
 * content for one product: payoff, assumptions, how it's priced by this
 * codebase's engines, and useful-to-know notes.
 *
 * Renders nothing when `doc` is undefined — that's what makes partial
 * content rollout safe: place this next to every product selector, and
 * only the products with an entry in productDocs.ts show an icon.
 */
export default function ProductInfo({ doc }: Props) {
	const [open, setOpen] = useState(false);
	const [pos, setPos] = useState({ top: 0, left: 0 });
	const triggerRef = useRef<HTMLButtonElement>(null);
	const panelRef = useRef<HTMLDivElement>(null);

	useEffect(() => {
		if (!open) return;

		const onDocClick = (e: MouseEvent) => {
			const target = e.target as Node;
			if (triggerRef.current?.contains(target) || panelRef.current?.contains(target)) return;
			setOpen(false);
		};
		const onKeyDown = (e: KeyboardEvent) => {
			if (e.key === "Escape") setOpen(false);
		};
		// Close on scroll rather than track it: the panel is `position: fixed`
		// off the trigger's viewport rect, so it would otherwise drift away
		// from the control it's explaining as the page scrolls underneath it.
		const onScroll = () => setOpen(false);

		document.addEventListener("mousedown", onDocClick);
		document.addEventListener("keydown", onKeyDown);
		window.addEventListener("scroll", onScroll, true);
		return () => {
			document.removeEventListener("mousedown", onDocClick);
			document.removeEventListener("keydown", onKeyDown);
			window.removeEventListener("scroll", onScroll, true);
		};
	}, [open]);

	if (!doc) return null;

	const toggle = () => {
		if (!open && triggerRef.current) {
			const rect = triggerRef.current.getBoundingClientRect();
			const left = Math.min(Math.max(16, rect.left), window.innerWidth - PANEL_WIDTH - 16);
			setPos({ top: rect.bottom + 8, left });
		}
		setOpen((o) => !o);
	};

	const payoffs = Array.isArray(doc.payoff) ? doc.payoff : [doc.payoff];

	return (
		<>
			<button
				ref={triggerRef}
				type="button"
				className="product-info-trigger"
				aria-label={`About ${doc.title}`}
				aria-expanded={open}
				onClick={toggle}
			>
				i
			</button>
			{open &&
				createPortal(
					<div
						ref={panelRef}
						className="product-info-panel card"
						style={{ top: pos.top, left: pos.left, width: PANEL_WIDTH }}
						role="dialog"
						aria-label={doc.title}
					>
						<div className="product-info-header">
							<h4>{doc.title}</h4>
							<button type="button" className="btn-icon" aria-label="Close" onClick={() => setOpen(false)}>
								✕
							</button>
						</div>

						<p className="product-info-summary">{doc.summary}</p>

						<div className="product-info-section">
							<div className="product-info-heading">Payoff</div>
							{payoffs.map((tex, i) => (
								<Formula key={i} tex={tex} block />
							))}
						</div>

						<div className="product-info-section">
							<div className="product-info-heading">Assumptions</div>
							<ul>
								{doc.assumptions.map((a, i) => (
									<li key={i}>{renderMathText(a, `a${i}`)}</li>
								))}
							</ul>
						</div>

						<div className="product-info-section">
							<div className="product-info-heading">How it's priced here</div>
							<ul>
								{doc.pricingMethods.map((m, i) => (
									<li key={i}>
										<strong>{m.engine}:</strong> {renderMathText(m.detail, `m${i}`)}
									</li>
								))}
							</ul>
						</div>

						<div className="product-info-section">
							<div className="product-info-heading">Useful to know</div>
							<ul>
								{doc.notes.map((n, i) => (
									<li key={i}>{renderMathText(n, `n${i}`)}</li>
								))}
							</ul>
						</div>
					</div>,
					document.body,
				)}
		</>
	);
}
