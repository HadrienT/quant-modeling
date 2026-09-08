/**
 * Copy text to the clipboard, resolving to whether it actually worked.
 *
 * `navigator.clipboard` only exists in a secure context — HTTPS, or `localhost`.
 * Reached over plain HTTP on a LAN address (e.g. http://192.168.1.200:5180) it
 * is `undefined`, so the async Clipboard API silently does nothing. We fall back
 * to a hidden `<textarea>` + `document.execCommand("copy")`, and when even that
 * fails we return `false` so the caller can tell the user instead of showing a
 * "copied!" toast that was a lie.
 */
export async function copyText(text: string): Promise<boolean> {
	try {
		if (window.isSecureContext && navigator.clipboard?.writeText) {
			await navigator.clipboard.writeText(text);
			return true;
		}
	} catch {
		/* fall through to the legacy path */
	}

	try {
		const ta = document.createElement("textarea");
		ta.value = text;
		ta.setAttribute("readonly", "");
		ta.style.position = "fixed";
		ta.style.top = "-9999px";
		document.body.appendChild(ta);
		ta.select();
		const ok = document.execCommand("copy");
		document.body.removeChild(ta);
		return ok;
	} catch {
		return false;
	}
}
