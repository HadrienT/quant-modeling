/** The reason the server gives in the diagnostics when it priced the
 * first order but not the second. */
export function secondOrderRefusal(diagnostics: string): string | null {
	const m = /no second-order risks \((.+)\)/.exec(diagnostics);
	return m ? m[1]! : null;
}
