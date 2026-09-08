import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import { describe, expect, it } from "vitest";
import { ROUTES } from "@/app/router";

/**
 * Architecture-discipline tests — blueprint WP 12 §3. Cheap, and they stop the
 * structure dissolving in six months. They complement the eslint
 * import/no-restricted-paths rule (which enforces the dependency direction).
 */

const SRC = join(__dirname, "..", "src");

function walk(dir: string): string[] {
	const out: string[] = [];
	for (const entry of readdirSync(dir)) {
		const p = join(dir, entry);
		if (statSync(p).isDirectory()) out.push(...walk(p));
		else if (/\.(ts|tsx)$/.test(entry)) out.push(p);
	}
	return out;
}

const files = walk(SRC).filter((f) => !/\.(test|spec|stories)\.tsx?$/.test(f));
const read = (f: string) => readFileSync(f, "utf8");
const rel = (f: string) => relative(SRC, f);

describe("no wild fetch", () => {
	it("only shared/api and shared/config call fetch()", () => {
		const offenders = files.filter((f) => {
			if (/shared[/\\](api|config|test)[/\\]/.test(f)) return false;
			return /\bfetch\(/.test(read(f).replace(/\/\/.*$/gm, ""));
		});
		expect(offenders.map(rel)).toEqual([]);
	});
});

describe("no hard-coded colours outside theme.css", () => {
	it("no hex literals in .ts/.tsx", () => {
		const HEX = /#[0-9a-fA-F]{3,8}\b/;
		const offenders = files.filter((f) => {
			// the CSS→JS bridges' defensive fallbacks are the sanctioned exception
			if (/shared[/\\]viz[/\\]theme\.ts$/.test(f)) return false;
			if (/shared[/\\]styles[/\\]tokens\.ts$/.test(f)) return false;
			if (/scripts[/\\]/.test(f)) return false;
			return read(f)
				.split("\n")
				.some((line) => HEX.test(line) && !line.includes("eslint"));
		});
		expect(offenders.map(rel)).toEqual([]);
	});
});

describe("no console.log outside a logger", () => {
	it("uses console.warn/error only", () => {
		const offenders = files.filter((f) => /\bconsole\.log\(/.test(read(f)));
		expect(offenders.map(rel)).toEqual([]);
	});
});

describe("dependency direction", () => {
	it("shared/ never imports features/ or app/", () => {
		const offenders = files
			.filter((f) => /[/\\]shared[/\\]/.test(f))
			.filter((f) => /from ["']@\/(features|app)\//.test(read(f)));
		expect(offenders.map(rel)).toEqual([]);
	});

	it("a feature never imports another feature", () => {
		const offenders: string[] = [];
		for (const f of files) {
			const m = f.match(/features[/\\]([^/\\]+)[/\\]/);
			if (!m) continue;
			const own = m[1];
			const imports = [...read(f).matchAll(/from ["']@\/features\/([^/"']+)/g)];
			for (const im of imports)
				if (im[1] !== own) offenders.push(`${rel(f)} → features/${im[1]}`);
		}
		expect(offenders).toEqual([]);
	});
});

describe("routes are exhaustive", () => {
	it("every declared route has a label", () => {
		for (const r of ROUTES) {
			expect(r.path).toMatch(/^\//);
			expect(r.label.length).toBeGreaterThan(0);
		}
	});
});

describe("feature files stay small (WP 07 / 11 pitfalls)", () => {
	it("no feature file exceeds 230 lines", () => {
		const big = files
			.filter((f) => /[/\\]features[/\\]/.test(f))
			.map((f) => [rel(f), read(f).split("\n").length] as const)
			.filter(([, n]) => n > 230);
		expect(big).toEqual([]);
	});
});
