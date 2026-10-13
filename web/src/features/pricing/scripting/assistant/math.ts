const display = (tex: string) => `\n\n$$\n${tex.trim()}\n$$\n\n`;

/** Index of the `$` closing the inline formula opened at `open`, or -1. Pandoc's
 * rule, which is what keeps "$100 and $130" as two prices: the opening dollar
 * is followed by a non-space, the closing one is preceded by a non-space and
 * not followed by a digit. */
function closingDollar(text: string, open: number): number {
	if (/\s/.test(text[open + 1] ?? " ")) return -1;
	for (let j = open + 1; j < text.length; j++) {
		const ch = text[j];
		if (ch === "\\") j++;
		else if (ch === "\n" && text[j + 1] === "\n") return -1;
		else if (ch === "$") {
			const closes =
				!/\s/.test(text[j - 1] ?? "") && !/[\d$]/.test(text[j + 1] ?? "");
			return closes ? j : -1;
		}
	}
	return -1;
}

/**
 * Rewrite the LaTeX of an assistant reply into the one form remark-math reads:
 * `$…$` inline, a `$$` fence on its own lines for display. Models also write
 * `\(…\)` and `\[…\]`, which Markdown would otherwise read as escaped brackets.
 * A dollar that opens no formula — a price, or a formula still being streamed —
 * is escaped, so it shows as a dollar instead of swallowing the text after it.
 * Code spans are left alone.
 */
export function normalizeMath(text: string): string {
	let out = "";
	let i = 0;
	while (i < text.length) {
		const c = text[i];
		if (c === "`") {
			let run = 1;
			while (text[i + run] === "`") run++;
			const close = text.indexOf("`".repeat(run), i + run);
			const end = close === -1 ? i + run : close + run;
			out += text.slice(i, end);
			i = end;
		} else if (c === "\\") {
			const next = text[i + 1];
			const close =
				next === "(" || next === "["
					? text.indexOf(next === "(" ? "\\)" : "\\]", i + 2)
					: -1;
			if (close === -1) {
				out += text.slice(i, i + 2);
				i += 2;
			} else {
				const tex = text.slice(i + 2, close);
				out += next === "(" ? `$${tex.trim()}$` : display(tex);
				i = close + 2;
			}
		} else if (c === "$" && text[i + 1] === "$") {
			const close = text.indexOf("$$", i + 2);
			if (close === -1) {
				out += "\\$\\$";
				i += 2;
			} else {
				out += display(text.slice(i + 2, close));
				i = close + 2;
			}
		} else if (c === "$") {
			const close = closingDollar(text, i);
			if (close === -1) {
				out += "\\$";
				i++;
			} else {
				out += text.slice(i, close + 1);
				i = close + 1;
			}
		} else {
			out += c;
			i++;
		}
	}
	return out;
}
