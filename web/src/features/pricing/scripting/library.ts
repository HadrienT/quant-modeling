/**
 * The product library of the scripting page: one readable, commented script
 * per product in ./library/*.qms, each opening with a header the page reads
 *
 *   # title: Worst-of call
 *   # category: Multi-asset
 *   # underlyings: 3
 *   # source: ...          (one line per source, at least one)
 *   # summary: ...
 *
 * The header is shown next to the editor and removed from the script loaded
 * into it. api/tests/test_script_library.py parses and prices every file.
 */

const RAW = import.meta.glob<string>("./library/*.qms", {
	query: "?raw",
	import: "default",
	eager: true,
});

export type LibraryProduct = {
	slug: string;
	title: string;
	category: string;
	underlyings: number;
	sources: string[];
	summary: string;
	script: string;
};

export const CATEGORY_ORDER = [
	"Vanillas and digitals",
	"Barriers and touch options",
	"Asians, lookbacks and ladders",
	"Cliquets",
	"Structured notes",
	"Volatility",
	"Multi-asset",
] as const;

const META = /^# (title|category|underlyings|source|summary): (.+)$/;

export function parseProduct(slug: string, raw: string): LibraryProduct {
	const p: LibraryProduct = {
		slug,
		title: slug,
		category: "",
		underlyings: 1,
		sources: [],
		summary: "",
		script: "",
	};
	const body: string[] = [];
	for (const line of raw.split("\n")) {
		const m = META.exec(line);
		if (!m) {
			body.push(line);
			continue;
		}
		const [, key, value] = m;
		if (key === "source") p.sources.push(value!.trim());
		else if (key === "underlyings") p.underlyings = Number(value);
		else if (key === "title" || key === "category" || key === "summary")
			p[key] = value!.trim();
	}
	// Drop the bare "#" lines left between the header and the comments.
	while (body.length && /^#?\s*$/.test(body[0]!)) body.shift();
	p.script = body.join("\n");
	return p;
}

const rank = (c: string) =>
	(CATEGORY_ORDER as readonly string[]).indexOf(c) >>> 0;

export const SCRIPT_LIBRARY: LibraryProduct[] = Object.entries(RAW)
	.map(([path, raw]) =>
		parseProduct(path.replace(/^.*\/|\.qms$/g, ""), raw as string),
	)
	.sort(
		(a, b) =>
			rank(a.category) - rank(b.category) || a.slug.localeCompare(b.slug),
	);

export const DEFAULT_PRODUCT =
	SCRIPT_LIBRARY.find((p) => p.slug === "european-call") ?? SCRIPT_LIBRARY[0]!;
