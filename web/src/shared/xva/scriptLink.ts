/**
 * A rate script carried from the Scripting page to the xVA page in the URL
 * (WP 23, K4): base64url of its text, so that the netting set of one's own
 * script is a link like any other view of the page.
 */

/** The API's bound on a scripted trade (ScriptTradeInput.script). */
export const MAX_SCRIPT = 4000;

export function encodeScript(script: string): string {
	return btoa(unescape(encodeURIComponent(script)))
		.replace(/\+/g, "-")
		.replace(/\//g, "_")
		.replace(/=+$/, "");
}

export function decodeScript(text: string | undefined): string | null {
	if (!text) return null;
	try {
		return decodeURIComponent(
			escape(atob(text.replace(/-/g, "+").replace(/_/g, "/"))),
		);
	} catch {
		return null;
	}
}

/** A script the exposure engine can value: it reads the discount curve and
 * holds no equity underlying (rates only). */
export const isRateScript = (script: string, underlyings: number) =>
	/\bdf\s*\(/.test(script) && underlyings === 0 && script.length <= MAX_SCRIPT;
