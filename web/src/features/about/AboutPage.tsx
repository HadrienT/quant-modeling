import { getConfig } from "@/shared/config";
import { useHealth } from "@/shared/api";

export default function AboutPage() {
	const health = useHealth();
	const cfg = getConfig();

	return (
		<div className="prose-invert mx-auto max-w-2xl text-sm text-ink-secondary">
			<h1 className="text-lg font-semibold text-ink">Quant Modeling</h1>
			<p className="mt-3">
				A derivatives-pricing library in C++20 exposed all the way to a usable
				product: <code>include/quantModeling</code> + <code>src</code> →
				pybind11 bindings → a FastAPI service → this React front end.
			</p>
			<p className="mt-3">
				The C++ split is a desk library's: <b>payoff</b> (instruments),{" "}
				<b>model</b>, <b>numerical method</b> (analytic / tree / PDE / Monte
				Carlo), <b>orchestration</b> (pricers with a registry). No engine
				branches on a product type; no instrument reads market data.
			</p>
			<h2 className="mt-6 text-sm font-semibold text-ink">This front end</h2>
			<ul className="mt-2 list-disc pl-5">
				<li>
					Types generated from the FastAPI OpenAPI schema, with a CI drift test
				</li>
				<li>
					TanStack Query for server state, typed URL search params for view
					state — a pricing session or a strategy is a shareable link
				</li>
				<li>
					A generic WebGL surface engine (three.js / R3F): implied vol, local
					vol and P&amp;L nappes are the same object; holes are rendered as
					absence, never interpolated
				</li>
				<li>
					Dark by default, full light theme, colour-blind-validated palette
				</li>
			</ul>
			<h2 className="mt-6 text-sm font-semibold text-ink">Build</h2>
			<dl className="mt-2 grid grid-cols-[auto_1fr] gap-x-4 font-mono text-xs">
				<dt className="text-ink-muted">commit</dt>
				<dd>{cfg.commitSha}</dd>
				<dt className="text-ink-muted">API</dt>
				<dd>
					{health.isLoading
						? "…"
						: health.isError
							? "unreachable"
							: `online · ${health.data?.version ?? "?"}`}
				</dd>
			</dl>
			<p className="mt-6">
				<a
					className="text-accent hover:underline"
					href="https://github.com/HadrienT/quant-modeling"
					target="_blank"
					rel="noreferrer"
				>
					Source on GitHub
				</a>
			</p>
		</div>
	);
}
